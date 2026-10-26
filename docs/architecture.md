# pico-http Architecture

How the pico-http static core works, layer by layer. Requirement IDs
(`AR-…`, `TR-…`, `BR-…`, …) reference the
[technical specification](technical-specification.md); this document describes
the implementation as shipped.

```
             handlers (your code)
                    │
   ┌────────────────▼─────────────────────┐
   │ HttpService              src/http/   │  routing · incremental parsing ·
   │   HttpParser · ResponseWriter        │  streaming responses · keep-alive ·
   │   RouteTable · message types         │  per-phase timeouts · statistics
   └────────────────┬─────────────────────┘
                    │  ITcpTransport (pure C++, no lwIP types)
   ┌────────────────▼─────────────────────┐
   │ transport core            src/tcp/   │  ConnectionId · SlotTable ·
   │                                      │  TimeoutScheduler
   ├──────────────────────────────────────┤
   │ LwipTcpTransport     src/tcp/lwip/   │  firmware: lwIP raw API, NO_SYS=1
   │ FakeTcpTransport     src/testing/    │  host: simulated wire for tests
   └──────────────────────────────────────┘

   src/config/   sizing profile, boundary limits, slot count  (every sizing layer)
   src/util/     TxRing  (FixedString and as_bytes are pico-util's)
   src/lwipopts/ what is required of the application's lwipopts.h (firmware)
   src/lwipopts_reference/
                 a reference configuration meeting it (firmware)
   src/include/  pico_http.hpp umbrella and version constants
```

Every target that sizes storage depends on `src/config/`, which owns each constant that determines
static storage: the slot count, the MSS-derived buffer extents, the header caps
and the parser's boundary limits. It carries the profile selection as a usage
requirement, so a consumer compiles its own translation units against the same
layout as the library it links — a profile that fails to propagate is a compile
error rather than a silent mismatch. The lwIP configuration itself is the
application's (AR-10); `src/lwipopts/` says what this library requires of one,
and `src/lwipopts_reference/` holds a configuration meeting it. The profile and that configuration are bound
by compile-time assertions — the advertised receive window must equal
`TCP_WND` — checked by a host test under both profiles.

The seam is load-bearing: everything above `ITcpTransport` is pure C++ with no
lwIP dependency, so the entire HTTP layer — parsing, backpressure, timeouts,
error paths — runs under GoogleTest on the host against `FakeTcpTransport`.
Only the lwIP glue needs hardware.

## Connection identity and slots

Connections are named by `ConnectionId`, a packed `{slot:8, generation:16,
tag:8}` integer — never a raw `tcp_pcb*` (AR-4). `SlotTable` owns a fixed
array of per-connection slots; releasing a slot bumps its generation, so any
retained copy of an old id fails generation validation and every operation on
it becomes an inert no-op. This makes the lwIP dangling-pcb bug class
structurally impossible: there is no way to name a connection that no longer
exists.

Both layers keep fixed slot arrays sized to `MAX_CONNECTIONS` (8):

- **Transport slot** (`LwipTcpTransport::Slot`): the `tcp_pcb*`, a `TxRing`,
  and drain-close state.
- **Service slot** (`HttpService::Conn`): parser state, the RX assembly
  buffer, response-writer state, keep-alive counters.

When the table is full, the 9th connection is refused at the TCP layer with an
RST before any HTTP state is created (RR-3); the `on_refused` callback feeds
the `refusals_slot_full` counter.

## Teardown: one path for every exit

All connection exits — peer FIN, peer RST, every timeout kind, drain-close
completion, `close_server()` — converge on `TransportBase::teardown()`, which
disarms the slot's timeout, has `LwipTcpTransport::release_slot_resources()`
detach the lwIP callbacks, close or abort the pcb and clear the TX ring,
releases the slot (bumping the generation), and fires `on_disconnect` with a
typed `DisconnectReason`. Two lwIP subtleties are
handled here:

- **`tcp_err` means the pcb is already freed.** The error callback clears the
  slot's `pcb` before tearing down, so the shared teardown makes no `tcp_*`
  call on it. `tcp_arg` points
  at the *slot* (not the server), so this path reaches per-connection state in
  O(1).
- **In-callback aborts must return `ERR_ABRT`.** When teardown aborts a pcb
  from inside that pcb's own callback, the abort is latched and the callback
  returns `ERR_ABRT` via `consume_abort()`; aborts issued outside any callback
  (timeout worker, direct close) are deliberately not latched so a stale
  pointer can never alias a reused pcb address.

Because the TX ring is handed to lwIP with NOCOPY writes, a teardown while
data is still pending must `tcp_abort()` (RST) rather than `tcp_close()` —
a graceful close would leave lwIP retransmitting from ring memory about to be
reused by the next connection. The orderly path (`close_client` →
drain-then-close) only reaches teardown once the ring is empty, so normal
closes remain graceful.

## RX path (AR-6: flow control, never drops)

```
lwIP pbuf ──► on_receive ──► Conn::rx assembly buffer ──► HttpParser
                                    │
                    consume(n) after the parser takes n bytes
                                    │
                            tcp_recved(n) — reopens the window
```

Received bytes are copied once into the slot's RX assembly buffer
(`RX_CAPACITY` = `ADVERTISED_RX_WINDOW`: 2×MSS on the default profile, 4×MSS
on P8-RP2350; sized ≥ the advertised TCP window and static_asserted against
it) and
acknowledged to lwIP **only after the parser consumes them**. Unconsumed bytes
— a partial request head, or pipelined requests queued behind a response that
is still draining — hold the receive window closed instead of being dropped.
The invariant `RX_CAPACITY ≥ TCP_WND` guarantees a full window's delivery
always fits, so no delivered byte is ever discarded.

`HttpParser` is an incremental, single-pass state machine: it accepts
arbitrarily segmented input, consumes each byte exactly once (no O(n²)
rescans), and enforces every boundary limit *during* parsing — request-line
and URI caps (414), per-line/count/total header caps (431), body caps (413),
malformed syntax (400), unmet expectations (417), unsupported transfer codings
(501), unsupported versions (505). Repeated header names combine into one
comma-separated value (NR-10); exactly one empty line before the request-line
is tolerated (NR-1). Chunked `Transfer-Encoding` is decoded natively (strict
`1*HEXDIG` chunk sizes, overflow-checked, extensions and trailers skipped —
control octets rejected — within an overhead budget); `Content-Length` +
`Transfer-Encoding` together, or `Transfer-Encoding` on an HTTP/1.0 request,
is rejected as a request-smuggling shape (NR-3/NR-4). Bytes past a completed
request are left in the buffer — that is how pipelining works.

## TX path (AR-5: backpressure, never truncation)

Each response is streamed by `ResponseWriter`, which the service pumps through
the transport's credit-based `send()`. The writer holds the serialized head,
the body source and both cursors, and advances through one explicit state, so a
stall has a single place to resume from:

1. Any partially-delivered interim `100 Continue` is flushed first (the two
   heads must never interleave).
2. The **head** (status line + headers) is serialized once into a fixed 1 KiB
   per-slot buffer — no heap — and streamed from a cursor. A head that
   overflows the buffer is replaced by a minimal `500` + `Connection: close`
   rather than sent truncated. A `HEAD` request runs the matching handler
   (falling back to the `GET` route, NR-9) and emits the head only — no body,
   no last-chunk.
3. The **body** streams from whichever source the handler chose:
   `STATIC_VIEW` (a view over caller memory, copied only into the TX ring,
   declared as a `StaticBody`
   so a request view cannot be handed to it -- AR-11), `GENERATOR` (a pure
   function of byte offset, pulled 512 B at a time into stack scratch), or
   `BUFFERED` (the slot's copied body, held until sent; see Allocation).

`send()` returns how many bytes the transport accepted (`0..n`); a short
return means the TX ring is full. `pump()` simply stops and resumes on the
next `on_writable` event, which the transport fires as peer ACKs free ring
space. Response size is therefore unbounded by memory: a 256 KB body streams
through a 4×MSS ring (verified bit-exact on hardware, PR-8).

The `TxRing` is the zero-copy contract with lwIP: `tcp_write()` is called
**without** `TCP_WRITE_FLAG_COPY`, referencing ring memory directly given
`LWIP_NETIF_TX_SINGLE_PBUF` 0, which AR-10 asserts, so bytes
move through three regions — written → in-flight → released — and are
reclaimed only when the peer ACKs them (retransmission may re-read them until
then).

## Timeouts (TR-1..6)

A 100 ms worker on the cyw43 async context linearly scans one
`{deadline, kind}` pair per slot (`TimeoutScheduler`; 8 slots do not justify a
timer wheel). Deadline arithmetic is wrap-safe signed-difference comparison,
correct across the 49.7-day `to_ms_since_boot` wrap (AR-7). The HTTP layer
arms the deadline matching the connection phase:

| Phase | Timeout | Default | Purpose |
|---|---|---|---|
| Reading head | TR-1 header | 5 s | Slowloris defense — a **fixed window from accept**, not reset by drip |
| Reading body | TR-2 body progress | 5 s | Minimum-data-rate proxy, re-armed on progress |
| Idle (keep-alive) | TR-3 idle | 15 s | Reclaims idle slots |
| Sending | TR-4 write stall | 10 s | Slow-reader bound, re-armed on ACK progress |
| Draining a close | TR-5 drain linger | 2 s | Armed by the transport itself in `close_client()` |
| Awaiting a deferred handler | TR-6 handler | 10 s | Bounds a handler that returns `PENDING` and never resolves (AR-9); expiry tears the connection down rather than synthesizing a response |

## Boundary behavior (BR-1..14)

Every boundary violation (BR-1..14) except the connection cap produces a real
HTTP status with `Connection: close` — never a silent drop, truncation, or opaque connection reset: `414`
(request line/URI), `431` (header line/count/total, or a combined
repeated-header value over the cap), `413` (body over the buffered cap, or
overflowing declared/chunk sizes), `400` (malformed syntax, duplicate or
conflicting framing headers), `417` (expectations other than `100-continue`),
`501` (transfer codings other than a lone `chunked`), `505` (well-formed but
unsupported HTTP versions), `503` (a deferral past the BR-12 cap), and `500` for
a handler whose response head overflows its buffer (BR-13) or whose copied body
exceeds the buffered-body cap (BR-14). The connection cap (BR-6) is the
exception: it is refused at the TCP layer, with no HTTP response. Limits are
compile-time policy (`DefaultHttpLimits` in
`src/config/include/pico_http/config/limits.hpp`); tests instantiate the parser
with tiny limits to hit every boundary deterministically.

## Errors

Three channels, and which one a failure uses follows from whose failure it is.

**The caller's failure is returned.** `feed()` returns a `ParseResult`, whose
non-zero error values *are* the HTTP status the service must send — the value
carries the recovery, not just the fact of failure. `set_body` returns a
`BodyResult`, below. Everything else that can be refused returns `bool`:
`start()` could not bind, `add_route()` was given a
method or pattern too long, `set()` had no room for a header. Those are
single-cause refusals, so a bool loses nothing; where a call has several distinct
causes and the caller's recovery differs between them, the cause belongs in the
return type rather than only in a log line.

`respond()` is the deliberate exception. It returns `bool` over six causes —
timed out, reset, reclaimed by `stop()`, moved on to a later request, already
completed, invalid id — because every one of them means the same thing to the
caller: the turn is gone, and there is nothing to retry (AR-4). Collapsing them
is the point, not an oversight.

**The peer's failure is counted.** A refused connection, a malformed request, a
timeout: none of these is a failure of the call that happened to observe them,
and there is often no call to return through. `statistics()` is the API for
those (OR-1..4).

**Nothing is silently dropped.** A refusal a caller could act on is reported to
that caller. `set_content_type()` returns whether the header fit, because a
response that lost its Content-Type is one a client may interpret differently,
and the handler that built an over-long type from request data is the only code
that can do anything about it. `set_body_static` and `set_body_generator`
propagate that `bool`, and set the body either way: a missing header is not a
reason to send nothing. `set_body` has two causes with different recoveries, so
it returns `HTTPResponse::BodyResult`: the body set, set without its
Content-Type, or refused over `MAX_BUFFERED_BODY` (BR-14). A refused body also
reaches the client as the fixed `500` and the device's statistics as
`responses_refused`.

**What this library does not do.** It defines no error enum of its own beyond
`ParseResult`, and re-exports none from pico-logging. `set_body`'s
`BodyResult` is an outcome rather than an error type: two of its three values
are a body that was set. `ParseResult` stays as it is rather than becoming a
`pico_util::Result` error type: its zero is
`INCOMPLETE` — a state, not success — and `is_error()` discriminates against
`FIRST_ERROR_STATUS` because the numeric value is the status code. A taxonomy
whose values are the protocol's own is not one to normalise.

## Observability (OR-1..4)

`HttpService::statistics()` exposes the counter set OR-1 defines, among them
accepts,
refusals, requests completed, parse errors by class, disconnects by reason
(including each timeout kind), backpressure events, RX-window throttle events,
byte counts, and the slot high-watermark. The benchmark firmware serves them
as JSON at `GET /stats`, the configured limits at `GET /limits`, and build
identity at `GET /info`. `pico_util::HeapWatermark` provides the post-init
heap-growth measurement backing the AR-1 "zero post-init heap" claim; the
utility and the requirement it discharges (pico_util:MR-1) live in pico-util,
which the
benchmark firmware links.

## Allocation

`HttpService` and the transport are constructed once, before `start()`. The
route table and the handler wiring allocate there, before `start()`. What
`AR-1` forbids is allocation on the steady-state paths afterwards, and a
heap watermark held inside AR-1's bound across the benchmark suite and the
soak is what establishes it rather than a code reading.

One bounded exception, copied-mode `set_body`, is stated in full in `AR-1`, with
its cap in BR-14. `set_body_static` and `set_body_generator` allocate nothing.

## What runs where

| Concern | Host-tested (GoogleTest) | Device-tested (bench.py) |
|---|---|---|
| Parser (incl. fuzz corpus) | ✔ `pico_http_http_tests`, `pico_http_parser_fuzz` | t10 boundary matrix |
| Slot/ID/timeout/ring primitives | ✔ `pico_http_tcp_tests`, `pico_http_util_tests` | — |
| HTTP service (backpressure, keep-alive, timeouts, errors) | ✔ against `FakeTcpTransport` | t1–t22 |
| lwIP glue (`LwipTcpTransport`) | — | t1 receive-callback timing (PR-2), t8 RST robustness, t9 slowloris, t11 256 KB download, t14 timeout precision, t22 every receive window held at once |

Hardware validation runs on a Pico 2 W; reference records are committed in
[baselines](../examples/bench/baselines/), and the on-device
[benchmark suite](../examples/bench/README.md) produces them.

## What this is not

HTTP/2 or later, client-side HTTP, WebSockets, mutual TLS, and NTP or
wall-clock time are absent by decision rather than by omission. Each would
change what the server is rather than extend it: HTTP/2 replaces the framing the
parser exists to do, a client inverts the connection model, WebSockets keep a
connection past the request that opened it, and wall-clock time is a dependency
this library deliberately does not carry — a consumer needing it owns that,
which is what pico-time is for.

Server-side TLS is outside the supported surface.


## Transmit-ring credit bounds

`flush_tx` calls `mark_sent` only after a successful `tcp_write`, with a count
bounded by the contiguous `peek` span, itself bounded by unsent bytes.
`sent_callback` passes the acknowledged byte count from the same PCB to `ack`.
lwIP's `tcp_sent` contract defines that count as bytes of sent data acknowledged
by the peer (`lwip/tcp.h`, SDK 2.3.0); this adapter is the only writer to that
PCB. The ring increments in-flight credit only on successful writes and
releases it only on these callbacks. Its count therefore covers each callback.
Teardown unregisters the callbacks before clearing the ring and aborts a PCB
that still references ring bytes, so a reused ring cannot receive stale credit.

The ring's saturating credit operations preserve their existing generic
semantics: an over-large credit cannot underflow a count or advance past live
storage. In this adapter the bounds above make saturation unreachable for a
conforming lwIP callback; clamping does not shorten network input or release
any still-unacknowledged bytes. This reasoning relies on lwIP's callback
contract, not on an assumption that arbitrary transport credit is trustworthy.
`OverAckIsClampedToInFlight` checks the defensive count invariant separately.
