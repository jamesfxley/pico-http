# pico-http Architecture

This guide describes the implementation. Requirement tags refer to the
[technical specification](technical-specification.md).

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

`src/config/` owns slot counts, buffer sizes, header caps and parser limits.
Targets inherit its profile definitions so consumers and the library share a
layout. `HttpService` aborts on a caller/library layout mismatch. The
application owns lwIP configuration; the reference module supplies an example
(AR-10). Compile-time assertions bind profile sizes to lwIP options. Both
profiles are host-tested, and the advertised receive window must equal
`TCP_WND`.

The HTTP layer has no lwIP dependency. Host tests exercise parsing,
backpressure, timeouts and errors through `FakeTcpTransport`. The lwIP adapter
requires hardware tests.

## Connection identity and slots

`ConnectionId` packs an 8-bit slot, 16-bit generation and 8-bit tag (AR-4).
`SlotTable` stores fixed connection slots and advances the generation on
release. A stale id fails validation until the generation wraps; raw PCB
pointers do not cross the public interface.

Both layers keep fixed slot arrays sized to `MAX_CONNECTIONS` (8):

- Transport slot (`LwipTcpTransport::Slot`): the `tcp_pcb*`, a `TxRing`,
  and drain-close state.
- Service slot (`HttpService::Conn`): parser state, the RX assembly
  buffer, response-writer state, keep-alive counters.

A connection beyond `MAX_CONNECTIONS` receives TCP RST before HTTP state is
created (RR-3). `on_refused` increments `refusals_slot_full`.

## Connection teardown

Peer FIN, peer RST, timeouts, drained close and `close_server()` all reach
`TransportBase::teardown()`. It disarms the deadline, releases stack
resources, advances the slot generation and reports a typed disconnect reason.
Two callback rules govern the lwIP release path:

- The PCB is already freed when `tcp_err` runs. The callback clears the slot
  pointer before teardown. The callback argument names the slot, giving
  constant-time access to connection state.
- A callback that aborts its own PCB must return `ERR_ABRT`. `consume_abort()`
  reports the latched self-abort. External aborts leave no latch that could
  alias a reused PCB address.

A teardown with pending NOCOPY data must call `tcp_abort()`. A graceful close
would leave lwIP retransmitting from ring memory about to be reused. Normal
drain-close reaches teardown with an empty ring and closes gracefully.

## RX path

```
lwIP pbuf ──► on_receive ──► Conn::rx assembly buffer ──► HttpParser
                                    │
                    consume(n) after the parser takes n bytes
                                    │
                            tcp_recved(n) — reopens the window
```

Received bytes enter the slot RX buffer and return receive-window credit only
after parsing (AR-6). The buffer covers the advertised window: 2×MSS by default
or 4×MSS for P8-RP2350. A `static_assert` keeps `RX_CAPACITY` ≥ `TCP_WND`, so a
full window always fits and no delivered byte is dropped. Bytes behind a
draining response or deferred handler, whole requests or a partial head, stay
unparsed and hold the window closed.

`HttpParser` consumes segmented input in one pass and checks capacities while
parsing. It returns 414 for request-line or URI overflow, 431 for header limits,
413 for body overflow, 400 for malformed syntax, 417 for unsupported
expectations, 501 for unsupported transfer codings and 505 for unsupported
versions. Repeated fields combine as lists (NR-10). One leading CRLF is accepted
(NR-1). Chunked decoding checks sizes and controls while skipping extensions and
trailers within an overhead budget. Ambiguous Content-Length plus
Transfer-Encoding framing and HTTP/1.0 Transfer-Encoding are rejected (NR-3,
NR-4). Input after a complete request remains for the next request.

## TX path

`ResponseWriter` retains the serialised head, body source and cursors. Its
state records where output resumes after backpressure (AR-5):

1. Any partially-delivered interim `100 Continue` is flushed first (the two
   heads must never interleave).
2. The head (status line + headers) is serialised once into a fixed 1 KiB
   per-slot buffer — no heap — and streamed from a cursor. A head that
   overflows the buffer is replaced by a minimal `500` + `Connection: close`
   rather than sent truncated. A `HEAD` request runs the matching handler
   (falling back to the `GET` route, NR-9) and emits the head only — no body,
   no last-chunk.
3. The body streams from whichever source the handler chose:
   `STATIC_VIEW` (a view over caller memory, copied only into the TX ring,
   declared as a `StaticBody`
   so a request view cannot be handed to it -- AR-11), `GENERATOR` (a pure
   function of byte offset, pulled 512 B at a time into stack scratch), or
   `BUFFERED` (the slot's copied body, held until sent; see Allocation).

`send()` returns accepted bytes. A short return stalls the writer until
`on_writable` reports ACK progress. A body larger than the ring streams in
bounded storage (PR-8).

`tcp_write()` references ring bytes without `TCP_WRITE_FLAG_COPY`, so `TxRing`
retains them until ACK because retransmission can read them again.
`LWIP_NETIF_TX_SINGLE_PBUF` is zero so lwIP does not force copying
(AR-10). Written bytes become in-flight and are released only on ACK.

`StaticBody` exposes an explicit lifetime declaration. A literal-array
factory would require exceptions for the C-array and array-decay checks.
The declaration keeps that claim visible without those exceptions.

## Timeouts

A 100 ms async-context worker scans one deadline per slot. A linear scan
suffices for eight slots. Signed-difference arithmetic handles the 32-bit
millisecond wrap (AR-7). The HTTP layer arms the deadline for the connection
phase:

| Phase | Timeout | Default | Purpose |
|---|---|---|---|
| Reading head | TR-1 header | 5 s | Slowloris defence — a fixed window from accept, not reset by drip |
| Reading body | TR-2 body progress | 5 s | Minimum-data-rate proxy, re-armed on progress |
| Idle (keep-alive) | TR-3 idle | 15 s | Reclaims idle slots |
| Sending | TR-4 write stall | 10 s | Slow-reader bound, re-armed on ACK progress |
| Draining a close | TR-5 drain linger | 2 s | Armed by the transport itself in `close_client()` |
| Awaiting a deferred handler | TR-6 handler | 10 s | Bounds a handler that returns `PENDING` and never resolves (AR-9); expiry tears the connection down rather than synthesizing a response |

## Boundary behaviour

Boundary errors produce a status and `Connection: close`, except
connection-cap rejection, which sends TCP RST without HTTP (BR-1..14, BR-6).
Parser limits return 414, 431 or 413; malformed requests return 400; unmet
expectations return 417; unsupported codings and versions return 501 and 505.
Excess deferrals return 503 (BR-12). Oversized response heads or copied bodies
return 500 (BR-13, BR-14). Limits come from `DefaultHttpLimits`; tests use
small policies to reach each boundary.

## Errors

Caller errors use return values; peer errors use statistics.

`feed()` returns `ParseResult`. Its error values are the HTTP status the
service must send. `set_body()` returns `BodyResult`. Single-cause failures
return `bool`: bind failure, an overlong route or full header storage.
Distinct causes with different recovery belong in the return type.

`respond()` returns false for expired, reset, stopped, superseded, duplicate
or invalid requests (AR-4). All mean the caller has nothing to retry, so
separate error values would not change recovery.

A rejected connection, malformed request or timeout often has no caller to
return through. `statistics()` reports these events (OR-1..4).

Content-Type setters report header-capacity failure to the handler, since a
client can interpret an untyped body differently. Static and generator setters
return that result while setting the body either way. `set_body()`
distinguishes a copied body, a copied body without Content-Type, and an
oversized body rejected with no copy (BR-14). The last produces the fixed 500
and increments `responses_refused`.

The library re-exports no pico-logging error type. `BodyResult` records
outcomes, including two successful copies. `ParseResult` retains protocol
numeric values: zero means incomplete, and `is_error()` compares against
`FIRST_ERROR_STATUS`. It therefore cannot use the `pico_util::Result`
convention that zero means success.

## Observability

`statistics()` exposes connection, request, parser, timeout, flow-control and
byte counters (OR-1..4). The benchmark serves counters at `/stats`, limits at
`/limits` and identity at `/info`. Its pico-util `HeapWatermark`
measures post-init growth (AR-1, pico_util:MR-1).

## Allocation

The service, transport, routes and handler wiring allocate before `start()`.
The benchmark and soak check subsequent heap growth against the specification
budget (AR-1).

Copied `set_body()` is the bounded exception (AR-1, BR-14). Static and
generated bodies allocate nothing. Response entries contain several KiB of
headers, so the service-wide pool keeps them off the callback stack, and
boundary responses (every BR-1..BR-11 rejection that answers with a status,
and every `503`) write a fixed head into the connection's writer without
building an entry (AR-13). An entry
is held for one dispatch or completion, never across a deferral, so the pool
is sized by concurrent turns; the service counts the BR-12 deferral cap
separately. FlatMap keeps entries stationary. Releasing or resetting an entry
frees copied storage.
Header-line capacity includes the name and value caps plus colon and space, so
increasing value storage also increases accepted line length.

## Test coverage

| Concern | Host-tested (GoogleTest) | Device-tested (bench.py) |
|---|---|---|
| Parser (incl. fuzz corpus) | ✔ `pico_http_http_tests`, `pico_http_parser_fuzz` | t10 boundary matrix |
| Slot/ID/timeout/ring primitives | ✔ `pico_http_tcp_tests`, `pico_http_util_tests` | — |
| HTTP service (backpressure, keep-alive, timeouts, errors) | ✔ against `FakeTcpTransport` | t1–t22 |
| lwIP glue (`LwipTcpTransport`) | — | t1 receive-callback timing (PR-2), t8 RST robustness, t9 slowloris, t11 256 KB download, t14 timeout precision, t22 every receive window held at once |

Device validation uses Pico 2 W. The [benchmark](../examples/bench/README.md)
produces [baseline records](../examples/bench/baselines/); reference records
are deferred (FW-4).

## Scope exclusions

HTTP/2+, client-side HTTP, WebSockets, mutual TLS and wall-clock time are
outside scope. HTTP/2 changes framing, a client reverses connection ownership,
and WebSockets outlive a request. Applications needing wall-clock time can use
pico-time without adding it to this library's dependencies.

Server-side TLS is outside the supported surface (FW-3).


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

Saturating credit cannot underflow ring counts or advance beyond live storage.
The adapter bounds credit using successful writes and ACK callbacks, so
conforming lwIP callbacks never need saturation. `OverAckIsClampedToInFlight`
checks the defensive invariant. `ring_index` uses comparison instead of
division under the occupancy bound `head + offset < 2 * Capacity`.
