# pico-http Technical Specification

**Status:** Normative.

This specification defines the architecture, performance, reliability,
boundary-safety, and observability requirements for the pico-http TCP/HTTP
stack. Every requirement has an ID (`AR`/`PR`/`RR`/`BR`/`TR`/`NR`/`OR`) and is
mapped to an acceptance test in the traceability matrix (§12).
[architecture.md](architecture.md) describes the implementation; measured
validation records will live in
[examples/bench/baselines/](../examples/bench/baselines/); none is committed
yet (FW-4).
Cryptographic layers (TLS/HTTPS) are out of scope (§1; server-side TLS: FW-3).

---

## 1. Scope and targets

| | |
|---|---|
| Hardware targets | **Supported:** Pico 2 W (RP2350, 520 KB SRAM); its 0.1.0 validation record is pending (FW-4). The Pico W (RP2040) is refused at configure: pico-logging's atomics are not lock-free on ARMv6-M (FW-1). Both named profiles are sized for the Pico 2 W |
| Network stack | lwIP **raw API** only (`NO_SYS=1`, `LWIP_SOCKET=0`, `LWIP_NETCONN=0`) via `pico_cyw43_arch_lwip_threadsafe_background` |
| Protocol | HTTP/1.1 server subset defined in §9; HTTP/1.0 clients supported |
| Out of scope | TLS/HTTPS (server-side TLS: FW-3); HTTP/2+; client-side HTTP; **off-core handler execution** (AR-9's deferred-completion contract is written to admit it, but no off-core executor is provided (FW-2); lwIP raw is single-context regardless, so the transport, parser and response writer stay on the async-context core); NTP/wall-clock time (`to_ms_since_boot` suffices; the `pico-time` sibling stays unlinked) |

Two named configuration profiles parameterize every numeric limit in this document:

- **Default**: 8 concurrent connections, a 2 KiB buffered body and a 2xMSS receive
  window. All §3 arithmetic uses this profile.
- **P8-RP2350**: 8 concurrent connections, larger buffers (§3.4).

All limits are compile-time constants, set by the profile `PICO_HTTP_ACTIVE_PROFILE`
names and by the `PICO_HTTP_*` header caps (§3.5) and MSS factors (§3.4), so downstream projects
can define their own profile; the two named profiles are the ones this project validates.

**API stability:** the public API (`namespace pico_http`, headers under
`pico_http/`) follows semantic versioning; a breaking change lands at a minor
bump while the major version is 0, and at a major bump from 1.0.

Three API contracts referenced throughout this document:

1. `set_body_generator` without a length argument emits
   `Transfer-Encoding: chunked` to HTTP/1.1 clients and close-delimited framing
   to HTTP/1.0 clients (NR-6); the generator signals end-of-body by returning 0.
2. `ITcpTransport::send_credit(ConnectionId)` reports transmit-ring free space
   (`SIZE_MAX` when the transport does not model a ring). The response writer
   clamps each generator fill to it, so no byte is generated that the ring then
   rejects (bounds generator re-invocation under backpressure).
3. A request handler returns `Disposition::COMPLETE` when it has filled the
   response, or `Disposition::PENDING` when it will resolve the request later via
   `HttpService::respond(RequestId, ResponseFiller, void*)` (AR-9). The
   `HTTPResponse&` a handler receives is library-owned and valid only for the
   duration of the call; a deferring handler keeps the `RequestId` instead and
   fills the response through the filler the service invokes at completion.

## 2. Platform constraints and assumptions

These facts drive every number below:

1. **No scheduler.** `NO_SYS=1`: lwIP and all library callbacks run in the cyw43
   async-context (low-priority IRQ / alarm pool) path. There are no threads, no
   context switches, no RTOS primitives. A deferred handler (AR-9) does not
   change where work runs: `respond()` may be called from any context — core 1
   or an ISR — but it only records the completion and signals the async-context,
   which serializes and sends on its next turn like every other response. The one
   piece of state genuinely shared across contexts is the per-slot completion
   record, guarded by a critical section (AR-3); everything else stays
   single-context.
2. **Single lock.** All lwIP access is serialized by `cyw43_arch_lwip_begin()/end()`.
   User handlers must run outside that lock (AR-3).
3. **Time base.** `to_ms_since_boot(get_absolute_time())` provides a monotonic 1 ms
   tick. It wraps at 2³² ms ≈ 49.7 days; all deadline arithmetic must be wrap-safe
   (signed-difference comparison, AR-7).
4. **The radio is the bottleneck.** The cyw43 WiFi chip is attached over gSPI; practical
   TCP throughput over it is ~4–8 Mbit/s regardless of software quality, and LAN
   RTT over WiFi is typically 2–10 ms (both UNCONFIRMED: no source states them). Parsing a 1 KB request head at the RP2350's
   default 150 MHz (`SYS_CLK_HZ`, SDK `hardware/platform_defs.h`) with an
   O(n) parser costs tens of microseconds. **Performance targets are therefore
   network-bound**; the library's job is to never add milliseconds on top of the radio,
   and to stay predictable under load.
5. **No data cache in front of SRAM.** DMA/cache-line alignment concerns do not
   apply, and `MEM_ALIGNMENT 4` suffices. UNCONFIRMED for the RP2350: the SDK's
   register headers define a cache for XIP flash only, and the RP2350 datasheet
   that would settle it is not among this repository's sources.

## 3. Resource budgets

Storage divides into two pools with different lifetimes, and the two are budgeted
and gated separately:

- **Init-time allocation.** `HttpService` and the transport are constructed once,
  before `start()`, and hold every per-connection slot. AR-1 permits this; what it
  forbids is allocation on the steady-state paths afterwards.
- **Static RAM.** all `ALLOC` sections except `LOAD, READONLY` sections with
  equal VMA and LMA, `.heap` and `.stack*` reservations, dominated by the lwIP
  pools and the
  cyw43 driver. This is the pool the footprint gate measures.

Sizes below are derived from the compile-time profile (`config/profile.hpp`,
`config/limits.hpp`, `config/sizing.hpp`); each is stated as the expression that
produces it so a profile change cannot leave the budget behind.

### 3.1 Per-connection slot

One slot of `HttpService` (AR-1, AR-4):

| Item | Expression | Default | Rationale |
|---|---|---:|---|
| Parsed header storage | `sizeof(HTTPHeaders)` | 5,380 B | 16 headers at 64 B names / 256 B values, plus the count. The storage is `FixedString`, so each entry is its capacity rounded up to the alignment plus a length word: 16 x (72 + 264) = 5,376 B of entries. The largest per-slot knob (§3.5). |
| Buffered request body | `Limits::MAX_BUFFERED_BODY` | 2,048 B | BR-5. A larger body is refused with `413`; there is one body path and it is buffered. |
| Request-line storage | `sizeof(FixedString<MAX_METHOD + 1>) + sizeof(FixedString<MAX_URI + 1>) + sizeof(FixedString<MAX_VERSION + 1>)` | 568 B | BR-1. The caps are 16 / 512 / 16; the stored sizes are 24 / 520 / 24. |
| RX assembly buffer | `Profile::RX_CAPACITY` | 2,920 B | Must cover a full advertised window so no delivered byte is dropped (AR-6). |
| Serialized response head | `Profile::HEAD_CAPACITY` | 1,024 B | Status line and header section. A head over this is answered `500` and closed, never truncated (BR-13). |
| Parser and writer state | cursors, phase, keep-alive counters, and the slot's own padding | 676 B | The balance of `sizeof(Conn)` against the rows above, which is what makes the total the compiler's figure rather than a sum that can drift from it. |
| **Per-slot** | `HttpService::caller_binding().conn_size` | **12,616 B** | |
| **x `MAX_CONNECTIONS`** | | **98.6 KiB** | |

The figures are for the RP2350 target with the 64 B / 256 B header caps. The
benchmark image `static_assert`s the header storage, the request-line storage,
the per-slot size under both profiles (with §3.4's) and the TX ring below, so a
layout change fails its build until these tables move with it.

The transmit ring belongs to the transport's own slot, not to the service:

| Item | Expression | Both profiles |
|---|---|---:|
| TX ring | `TX_RING_CAPACITY` (`PICO_HTTP_TX_RING_MSS_FACTOR x MSS`) | 5,840 B |

Zero-copy `tcp_write` sources from the ring, so its bytes must stay valid until
acknowledged; its size bounds per-connection in-flight data. It is 4xMSS, the
reference `TCP_SND_BUF`, so the ring keeps as much in flight as lwIP will
(PR-5).

Response bodies are the one exception to fixed-capacity storage: `set_body`
copies the handler's bytes into one exact-sized heap buffer, which the slot takes
over rather than copies again (AR-1, AR-2), up to `MAX_BUFFERED_BODY` (BR-14). `set_body_static` and `set_body_generator` allocate nothing; the first takes a `StaticBody` rather than a bare span (AR-11).

Handlers are given an `HTTPResponse` from a service-level pool of
`MAX_PENDING_RESPONSES` objects, claimed at the start of a dispatch or a deferred
completion and released when that turn's response head has been serialized — never
held across a deferral (AR-9), because a deferring handler does not keep the
object. The pool is therefore sized by how many dispatch turns can be live at
once, not by how many requests are deferred. It is not one per slot: an
`HTTPResponse` carries a full `HTTPHeaders`, the dominant per-slot cost (§3.5), so
one per connection would add `MAX_CONNECTIONS x sizeof(HTTPResponse)` ~ 42 KiB to
the init-time pool.

A released entry is reset, so it holds no body buffer between turns: the
connection slot takes a copied body over from the entry when the response
begins (AR-2), and an entry that kept a buffer would hold copied-body heap that
no connection owns.

### 3.2 Network stack — what is required of the application's lwipopts

**This library does not own `lwipopts.h` and does not ship one** (AR-10). lwIP
admits exactly one configuration per image and the Pico SDK compiles the whole
lwIP core into whichever target links its INTERFACE libraries, so two libraries
each supplying one produce two lwIP builds whose struct layouts and memory-pool
tables disagree — resolved by archive order, and failing as silent memory
corruption a long way from the cause (`CXX-10.3`). The application names its
configuration in `PICO_LWIPOPTS_TARGET`.

What is normative is the *relation* each setting has to something this library
sizes from it, not a value. Each is a `static_assert` in
`lwipopts_assertions.hpp`, included by exactly one translation unit, so an
inadequate configuration is a compile error naming the setting.

| Requirement | Why |
|---|---|
| `NO_SYS == 1`, `LWIP_RAW == 1`, `LWIP_TCP == 1`, `LWIP_SOCKET == 0` | The transport is written against the raw `tcp_*` callbacks and driven from the SDK async context; there is no thread for a socket or netconn API. |
| `LWIP_NETIF_TX_SINGLE_PBUF == 0` | The TX path hands `tcp_write` a pointer into the connection's own ring without `TCP_WRITE_FLAG_COPY` (AR-2). At 1, lwIP forces that flag and copies every sent byte into its heap; lwIP's `opt.h` documents the option as a compatibility aid for DMA MACs without scatter-gather that a driver must not rely on. That the CYW43 driver needs none is UNCONFIRMED by its documentation. |
| `MEMP_NUM_PBUF >= MAX_CONNECTIONS * TCP_SND_QUEUELEN` | Each piece of ring queued takes a `PBUF_ROM` header, and `TCP_SND_QUEUELEN` caps the pbufs one connection may queue. Below it a write is refused and waits for the transport's poll, which appears as a stall rather than as a configuration error; lwIP's default of 16 is below what eight connections hold in flight. |
| `TCP_QUEUE_OOSEQ == 0` or `TCP_OOSEQ_MAX_PBUFS > 0` | An unbounded out-of-order queue lets a peer that withholds one segment pin RAM. |
| `MEMP_NUM_TCP_PCB >= MAX_CONNECTIONS` | Otherwise `accept()` starves before the service reaches its own connection limit. |
| `MEMP_NUM_TCP_PCB_LISTEN >= 1` | The service binds one listener. |
| `PBUF_POOL_SIZE * PBUF_POOL_BUFSIZE >= MAX_CONNECTIONS * TCP_WND` | The pool backs every connection's advertised window at once; below it the window is advertised and cannot be filled, which appears as a stall under concurrent upload rather than as a configuration error. |
| `ADVERTISED_RX_WINDOW == TCP_WND`, `TCP_MSS_BYTES == TCP_MSS`, `TX_RING_CAPACITY <= TCP_SND_BUF` | The host-safe mirrors this library sizes its buffers from must agree with the values lwIP was built against (AR-6). Asserted in `lwip_tcp_transport.cpp`. |

`TX_RING_CAPACITY` is 5,840 B (4×MSS) in both named profiles, so either needs
`TCP_SND_BUF` of at least 5,840 B; lwIP's own default is 2×MSS.

`src/lwipopts_reference/include/lwipopts_reference.h` is a configuration meeting
all of these, for an application to copy or to name directly through
`pico_http::lwipopts_fallback`. It is outside the automatic link graph: naming
it is what puts it on the include path, and nothing else does.

### 3.3 Totals and normative gates

**Normative requirements:**

- Static RAM (all `ALLOC` sections except `LOAD, READONLY` sections with equal
  VMA and LMA, `.heap` and `.stack*` reservations) <= **104 KiB** for the
  default
  profile and <= **130 KiB** for the P8-RP2350 profile, gated in CI by
  the footprint gate over two legs: `pico2_w` with each profile. The budget is
  keyed on the profile, not on the board.
- Flash (all `LOAD` sections) of the benchmark image <= **448 KiB**, gated by
  the
  same tool. Loose against the measured figure on purpose: this bounds a step
  change, not a few hundred bytes, and a gate that fails on noise gets raised
  until it means nothing. It covers the whole image, most of which is the SDK
  and the CYW43 firmware blob.
- Flash attributed to this repository's own sources <= **48 KiB**, gated by
  `owned_kib` in `pico.toml`. This is the figure a regression here can move: it
  does
  not shift when the SDK or the firmware blob does, and it is what a consumer
  adds by taking the library.
- Post-init heap allocation on the steady-state paths == **0** (AR-1).

Static RAM is dominated by the pbuf pool, the lwIP heap and the cyw43 driver; the
per-slot storage of §3.1 is init-time heap and is not part of it. Total RAM in use
is the sum of the two pools, against the RP2350's 520 KB.

### 3.4 P8-RP2350 profile

Raises the buffered body to 8,192 B (BR-5) and the receive window to 4xMSS (the TX
ring is 4xMSS in both profiles), paired with lwipopts `TCP_WND` 4xMSS and `PBUF_POOL_SIZE` 40. Header
caps, slot count and head capacity are unchanged from the default, giving a
per-slot 21,680 B and 169.4 KiB across eight slots — the same 676 B of parser
and writer state as the default, over a body of 8,192 B and a receive window of
5,840 B.

`P8Rp2350HttpProfile` (with `P8Rp2350HttpLimits`) carries the profile; select it
whole-build with `-DPICO_HTTP_PROFILE=p8-rp2350`, which defines
`PICO_HTTP_ACTIVE_PROFILE`, the buffer factors
`PICO_HTTP_RX_WINDOW_MSS_FACTOR` / `PICO_HTTP_TX_RING_MSS_FACTOR`, and
`PICO_HTTP_PROFILE_P8_RP2350` (which pairs the lwipopts values). Those definitions
are usage requirements of the `pico_http::config` target, so a consumer compiles
its own translation units against the same layout as the library it links; a
profile that fails to reach a consumer is a compile error there, not a silent
mismatch.

### 3.5 Header-storage knob

Parsed header storage is the dominant per-slot cost. The caps are **64 B names /
256 B values** (BR-2), shared by both profiles; values beyond the cap are rejected
with `431`, never silently truncated (BR-7). Both are set with the CMake cache
variables `PICO_HTTP_MAX_HEADER_NAME` and `PICO_HTTP_MAX_HEADER_VALUE`, which
put the value on `pico_http::config` as a usage requirement — they size storage
the library and its consumers must agree on, so a `-D` on a consumer's own
target is a layout mismatch rather than an override, and `ProfileBinding`
refuses it at construction (§3.4).

`Limits::MAX_HEADER_LINE` is derived from the two, so the enforced cap and the
advertised cap are the same number: raising the value cap raises what the parser
accepts rather than leaving storage no request can reach. `HttpService`
static_asserts the relation, so a downstream profile cannot set a line cap that
strands its own storage.

Each cap is the **usable length**, so a name or value of exactly that many
characters is stored whole and a caller may size a buffer against
`HTTPHeaders::MAX_HEADER_VALUE` and have the result fit. The underlying fixed
strings are declared one byte larger for their terminator. A rejected store
leaves the existing entry untouched and the header count unchanged — the
rejection and the storage never disagree.

## 4. Architecture requirements (AR)

| ID | Requirement |
|---|---|
| **AR-1** | **Zero post-init heap.** No heap allocation after `HttpService::start()` returns. Init-time allocation (route table, `std::function` wiring) is permitted and is budgeted separately from static RAM: it happens once, before `start()`, and is bounded by the route table the application declares. *Bounded exception:* copied-mode `set_body` allocates one exact-sized body vector per response; the connection slot takes it over when the response begins and frees it once the response is sent. A copied body over `MAX_BUFFERED_BODY` is never copied: `set_body` refuses it before allocating and the service answers `500` (BR-14), and a body a handler reserves or writes directly is refused over the cap, or trimmed to its size when its capacity exceeds the cap, before the slot takes it. Live copied-body allocation is therefore at most one body per connection while a response is in flight — hard-bounded by `MAX_CONNECTIONS × MAX_BUFFERED_BODY` — and zero once the connections are idle; a handler that reserves or writes `body` directly allocates what it asks for while it runs. Static and generator bodies allocate zero. That copied bodies leave nothing behind once sent is asserted on device (t18). Verified by a net-heap watermark — `pico_util::HeapWatermark` over mallinfo `uordblks` — held at zero growth across t15's static- and generator-body workload and inside the copied-body bound above across the soak (t7), not by a per-allocation hook: the Pico SDK exposes no malloc hook, so the measurable quantity is net growth, and an allocation freed before the sample is one it cannot see. That is also why the counter is a byte figure rather than an allocation count (OR-1). |
| **AR-2** | **Copy budget, bounded per direction.** RX: every received byte is copied once from its pbuf into the slot's RX assembly buffer (AR-6), which the parser reads; the request line and header bytes are copied through the parser's token scratch into the stored fields (§3.1), so a request view outlives the buffer's compaction (AR-9); a request body is copied once more into the parser's body buffer (BR-5); bytes not yet consumed are moved to the front of the assembly buffer as it drains; TX: `tcp_write` **without** `TCP_WRITE_FLAG_COPY` from the static TX ring (AR-10 asserts `LWIP_NETIF_TX_SINGLE_PBUF` 0, without which lwIP copies it again) (the 1 copy is body→ring, for every body source: a static body is copied from its storage, a generator's output from its scratch). Buffered mode adds, before that, `set_body`'s copy of the handler's bytes into its body buffer, which is the mode's purpose, and one exact-sized copy when a handler reserved that buffer past `MAX_BUFFERED_BODY` (AR-1); static and generator bodies make neither of those. Established by review of the RX and TX paths: no runtime copy counter exists, and `t17` compares the two body modes without counting copies. |
| **AR-3** | **Lock discipline.** All lwIP calls run under `cyw43_arch_lwip_begin()/end()`; user callbacks are invoked outside the lock. p99 lock hold time ≤ 500 µs, measured in a debug build during t5 load. The deferred-completion record (AR-9) is additionally guarded by a critical section, held only for the stores that publish or claim it — bounded, containing no lwIP call, and safe to take from an ISR. |
| **AR-4** | **Handle safety.** Public connection handles are `{slot index, generation}` integer IDs — never raw `tcp_pcb*`. Operations on stale handles are detected via generation mismatch and become inert no-ops (ABA-safe). |
| **AR-5** | **Backpressure, never truncation.** Response size is unbounded by memory: a resumable response writer driven by `tcp_sent` fills the TX ring as ACKs free space. A send is never silently dropped for want of room. |
| **AR-6** | **RX flow control.** `tcp_recved()` is called only for bytes actually consumed by the parser/handler. Unconsumed data closes the advertised window instead of being dropped, so silent truncation of delivered bytes is structurally impossible. |
| **AR-7** | **Wrap-safe millisecond time base.** All deadlines use `to_ms_since_boot` with signed-difference comparison, correct across the 49.7-day wrap. |
| **AR-8** | **Nagle disabled** (`tcp_nagle_disable`) on every accepted connection. Latency targets in §5 assume no Nagle/delayed-ACK interaction on small responses. |
| **AR-9** | **Deferred completion.** A handler may return `Disposition::PENDING` and resolve the request later via `respond(RequestId, ResponseFiller, void*)`. For the whole deferred window the request views (method, URI, headers, body) stay valid and the connection consumes no further pipelined request — buffered bytes hold the receive window closed against the peer exactly as they do behind a backpressured response (AR-6), so a deferring handler applies backpressure rather than accumulating work. The handler does **not** hold the response object across the window: it supplies a filler that the service invokes at completion time, and only if the request is still live, so a late completion cannot write into a recycled entry. `RequestId` is request-scoped — {generation-tagged connection id, request sequence} — so a completion arriving after the connection timed out, was reset, or was reclaimed by a server stop is inert (AR-4), and one arriving after that same connection has begun a later request cannot resolve that later request instead; a second `respond()` for the same request is likewise inert. `respond()` allocates nothing, makes no lwIP call and performs no send: it records the completion under a bounded critical section (AR-3) and signals the async-context, which sends on its next turn. It is therefore safe from any context, including core 1 and an ISR. The deferred window is bounded by TR-6, the number of concurrent deferrals by BR-12. |
| **AR-10** | **The application owns `lwipopts.h`.** This library asserts what it requires of a configuration and provides none. lwIP admits one configuration per image and the SDK compiles lwIP into whichever target links its INTERFACE libraries, so a library supplying one gives an application linking two networking libraries two lwIP builds whose struct layouts disagree, resolved by archive order (`CXX-10.3`). A firmware configure fails unless `PICO_LWIPOPTS_TARGET` names the application's target — the variable is the family's, so an application linking pico-http and a sibling names its configuration once. The requirements are §3.2, each a `static_assert` in a single translation unit, so an inadequate configuration is a compile error naming the setting rather than a fault under load. |
| **AR-11** | **A caller-owned body declares its lifetime in its type.** `set_body_static` takes a `StaticBody`, which only `StaticBody::outliving_response()` constructs, and whose name is the caller's assertion, written at the call site rather than made silently by a conversion. A `std::span` or `std::string_view` does not convert, so a view into `HTTPRequestView` cannot be served as a static body. That storage is the connection's parser buffer, recycled when the handler returns, and the writer re-reads a static body on every backpressure resume -- potentially long after that. |
| **AR-12** | **A board without an RP2350 and the CYW43 radio is refused at configure.** The server runs over the radio, and pico-logging needs the RP2350's exclusive access, so the library declares `PLATFORM rp2350 CYW43` to pico-tooling's bootstrap, which refuses a board without what a library declares. A consumer adding it on such a board fails at configure, naming what the board lacks, rather than in the build or on the device. The Pico W (RP2040) is therefore refused at configure (FW-1). |

## 5. Performance requirements (PR)

**Verification.** Network-measured performance is recorded and assessed at
review for trends and jumps against the recorded history, under matched profile
and build type with conditions (EN-06.7). Both client and board use Wi-Fi;
radio interference can move a single run's figures. The network numbers below
are documented expectations, not pass/fail limits, including the recorded 10%
reference-regression expectation. Acceptance evidence is a complete measurement
record with conditions and a review assessment of its trend and any unexplained
jump. The first recorded 0.1.0 runs begin that history. Device-internal processing
and lock timing retain their mechanised bounds. Functional robustness and
protocol timeouts retain their mechanised checks, including client-observed
recovery and closure checks; those are not speed expectations.

Latency percentiles: P50/P95/P99 (P99.9 for soak-length runs). Averages are not
acceptance criteria. **TTFB** and **TTLB** here are client-observed time to the first response byte
and to the complete response. The board-internal callback timing is recorded
separately.

| ID | Metric | Documented expectation / functional criterion | Test |
|---|---|---|---|
| PR-1 | Keep-alive `GET /ping` TTLB | p50 ≤ 8 ms, p99 ≤ 40 ms | t1 |
| PR-2 | TTFB, and internal processing share of it (parse + route + handler + serialize, timed per receive callback up to its first send) | p50 ≤ 6 ms; internal share ≤ 1 ms for ≥ 99 % of receive callbacks | t1 |
| PR-3 | Aggregate RPS, 8 keep-alive connections | ≥ 250 | t5 |
| PR-4 | Connection churn (connect + GET + close) | p50 ≤ 25 ms driven serially; ≥ 150 accepts/s driven by `MAX_CONNECTIONS` workers | t2 |
| PR-5 | Single-connection download throughput | p10 (nearest rank) of 20 transfers ≥ 400 KB/s at every response size from 16 KiB (theory: the 5,840 B ring / ~6 ms RTT ≈ 950 KB/s in flight, before the request's round trip and lwIP's 3×MSS initial window) | t3 |
| PR-6 | Aggregate download, `MAX_CONNECTIONS` connections | ≥ 600 KB/s excluding loss recovery (driver-bound) | t20 |
| PR-7 | Upload throughput (buffered body, at the `413` boundary) | ≥ 200 KB/s | t4 |
| PR-8 | No response-size ceiling | 256 KB download, bit-exact | t11 |
| PR-9 | Delay a burst adds, `MAX_CONNECTIONS` arriving together | Longest gap between received bytes on any connection ≤ 2 × the configured `LWIP_TCP_RTO_TIME`, plus one segment's delivery under a saturated cap | t20 |

## 6. Reliability and availability requirements (RR)

| ID | Requirement | Test |
|---|---|---|
| RR-1 | 100 consecutive peer RST/aborts mid-request leak **zero** slots; all 8 slots accept immediately afterwards, and `/stats` shows `active_connections <= 1` — the requesting connection is itself active, so 1 is the idle baseline — and `disconnect_peer_reset >= 98`, the ±2 covering a shot the device classifies as a plain close. | t8 |
| RR-2 | **Recovery SLO:** after any 1,000-event mixed-abuse run (RSTs, malformed frames, slowloris), a fresh client receives a `/ping` response within 1 s. | t21 |
| RR-3 | **Slot exhaustion:** the 9th concurrent connection is refused at the TCP layer (the accept callback aborts the pcb and returns `ERR_ABRT` → RST), counted in `refusals_slot_full`; the existing 8 connections are unaffected. | t5 |
| RR-4 | **Abuse soak:** zero heap drift beyond the bounded copied-body growth AR-1 permits, zero slot leaks, counter set self-consistent, and no allocation lwIP refuses from its heap or its pbuf, segment and `PBUF_ROM` pools. A run's record states its duration. | t7, t21 |
| RR-5 | Keep-alive reuse rate ≥ 99 % when the client requests it within configured limits. | t6 |
| RR-6 | **pbuf-pool pressure:** every slot's receive window full at once — the most the pool sizing AR-10 requires (§3.2) lets arrive, so the pool cannot be starved through receive windows — degrades to window throttling; never stream corruption, a dropped byte, a crash, or an allocation lwIP refuses from its heap or its pbuf, segment and `PBUF_ROM` pools. | t22 |

## 7. Boundary enforcement (BR)

Normative limits table. All boundary responses free the slot's buffers immediately,
include `Connection: close`, and close (or drain-close, TR-5) the connection.

The config column names where each limit is set. A profile supplies its own
`Limits` struct (§1) to change one; the two header caps additionally have CMake
cache variables, because they size storage the library and its consumers must
agree on and so cannot be set per-consumer.

| ID | Limit | Default | HTTP status | Config constant |
|---|---|---|---|---|
| BR-1 | Max request line (method + URI ≤ 512 B + version) | 576 B | `414` | `Limits::MAX_REQUEST_LINE` |
| BR-2 | Max single header line (64 name + 256 value + `: `) | 322 B | `431` | `Limits::MAX_HEADER_LINE`, derived from `PICO_HTTP_MAX_HEADER_NAME` and `PICO_HTTP_MAX_HEADER_VALUE` (CMake cache variables on `pico_http::config`) |
| BR-3 | Max header count | 16 | `431` | `Limits::MAX_HEADERS` |
| BR-4 | Max total request head | 2,048 B | `431` | `Limits::MAX_REQUEST_HEAD` |
| BR-5 | Max buffered body | 2,048 B | `413` | `Limits::MAX_BUFFERED_BODY` |
| BR-6 | Max concurrent connections | 8 | TCP-level refusal (no HTTP response) | `pico_http::MAX_CONNECTIONS` |
| BR-7 | Header storage overflow (`HTTPHeaders::set` failure) | — | `431` — **never a silent drop or truncation** | — |
| BR-8 | Malformed syntax (method/version charset, request-line spacing, header framing, control chars in a field value) | — | `400` | — |
| BR-9 | `Transfer-Encoding` other than a lone `chunked` | — | `501`. A lone `chunked` is decoded per §9; never misparsed as a zero-length body. | — |
| BR-10 | Missing or duplicated `Host` header on an HTTP/1.1 request (RFC 9112 §3.2) | — | `400` | — |
| BR-11 | Well-formed but unsupported HTTP version (`HTTP/<DIGIT>.<DIGIT>` that is not 1.0/1.1, e.g. `HTTP/2.0`) | — | `505` (RFC 9110 §15.6.6); a token that is not HTTP-version syntax is `400` (BR-8) | — |
| BR-12 | Max concurrent deferred requests (AR-9) | 4 | `503` | `Profile::MAX_PENDING_RESPONSES` |
| BR-13 | Max serialized response head (status line and header section a handler builds) | 1,024 B | `500` with `Content-Length: 0` and `Connection: close`, the handler's head and body dropped; never truncated, never a silent close | `Profile::HEAD_CAPACITY` |
| BR-14 | Max copied response body (`set_body`), held in the slot until sent | 2,048 B | `set_body` copies nothing and marks the response refused; the service answers the same fixed `500` as BR-13, counted in `responses_refused`. A larger body is served static or generated | `Limits::MAX_BUFFERED_BODY` |

BR-12 is enforced where a handler returns `PENDING`, not on arrival: a request
whose handler answers inline is never counted against it, so a saturated deferral
cap cannot refuse ordinary synchronous traffic. Boundary responses — BR-12's
`503` and the BR-1..BR-11 parse rejections — are built in a stack-local
`HTTPResponse` and serialized straight into the connection's response writer, so
they need neither a deferral slot nor the heap. BR-12 is an overload condition
rather than a malformed request, so it is counted separately (`deferrals_refused`,
OR-1) and never in `parse_errors`.

## 8. Timing and timeout requirements (TR)

All timeouts are enforced by a 100 ms periodic async-context worker linearly
scanning the slot deadlines; the requirement is the precision, not the data
structure. Each slot holds exactly one `{deadline_ms, kind}` pair, recomputed
on state transitions.

| ID | Timeout | Default | Precision | Purpose |
|---|---|---|---|---|
| TR-1 | Header completion (accept → head complete) | 5,000 ms | ± 200 ms | Slowloris defense |
| TR-2 | Body inter-progress (no forward progress while reading body) | 5,000 ms | ± 200 ms | Minimum-data-rate proxy |
| TR-3 | Keep-alive idle | 15,000 ms | ± 200 ms | Reclaims idle slots (configurable, existing API) |
| TR-4 | Write stall (no ACK progress while a response is pending) | 10,000 ms | ± 200 ms | Slow-reader bound |
| TR-5 | Drain-close linger | 2,000 ms | ± 200 ms | Replaces indefinite deferred close |
| TR-6 | Deferred handler completion (`PENDING` → `respond()`) | 10,000 ms | ± 200 ms | Bounds a handler that never resolves (AR-9) |

TR-6 expiry tears the connection down (reason `timeout_handler`) rather than
synthesizing a response: the handler still holds the response object, so writing
one would race it. The teardown bumps the connection generation, which is exactly
what makes the handler's eventual `respond()` inert (AR-4).

## 9. Protocol conformance (NR)

| ID | Requirement |
|---|---|
| NR-1 | Request-line parsing: exactly `METHOD SP URI SP VERSION CRLF`; method validated as an RFC 9110 token; overlong methods receive 400 because bounded token storage refuses them before dispatch (RFC 9112 §3 recommends 501); version parsed as `HTTP/<DIGIT>.<DIGIT>` — `1.0`/`1.1` accepted, other well-formed versions → `505` (BR-11), non-version tokens → `400` (BR-8). Exactly one empty line (CRLF) before the request-line is ignored (RFC 9112 §2.2); a second, or a bare LF, → `400`. |
| NR-2 | HTTP/1.0 vs 1.1 semantics: 1.1 default keep-alive, 1.0 default close, `Connection` header token parsing (case-insensitive, comma-separated). A response `Connection` header set by a handler overrides the version default; the per-connection request cap (`KeepAliveConfig::max_requests_per_connection`) and a framing decision that forces the connection closed both override the handler in turn — the cap bounds how long one peer holds a slot, so a handler must not be able to switch it off — and the emitted head carries the service's decision — a handler-set `Connection`/`Keep-Alive` on a response the service is closing is suppressed rather than echoed, so the head never announces persistence the server will not honour. |
| NR-3 | `Content-Length` hygiene: strict decimal parse with overflow check; duplicate or conflicting `Content-Length` headers → `400` (request-smuggling defense). `Content-Length` + `Transfer-Encoding` together → `400`. |
| NR-4 | `Transfer-Encoding` handling per BR-9: chunk-size hex parse with overflow bounds, trailers consumed and discarded, chunk extensions ignored (control octets in either → `400`). `Transfer-Encoding` on an HTTP/1.0 request is faulty framing → `400` (RFC 9112 §6.1). |
| NR-5 | `Expect: 100-continue`: send `100 Continue` when the declared body is within limits; otherwise respond `413` without reading the body. On HTTP/1.0 the 100-continue expectation is ignored and no 1xx is sent (RFC 9110 §10.1.1, §15.2). Any other expectation → `417` (RFC 9110 §10.1.1) — never silently ignored. |
| NR-6 | Response framing: every response carries an explicit `Content-Length`, or `Transfer-Encoding: chunked` for generator bodies of unknown length; HTTP/1.0 clients get length-delimited or close-delimited framing, never chunked. |
| NR-7 | `Host` enforcement (RFC 9112 §3.2): an HTTP/1.1 request must carry exactly one `Host` header; missing or duplicated `Host` → `400` (BR-10). HTTP/1.0 imposes no such requirement. |
| NR-8 | Method dispatch: a request whose URI matches a registered route only under other methods → `405` with an `Allow` header listing those methods (RFC 9110 §15.5.6, §10.2.1); a URI with no registered route → `404`. |
| NR-9 | `HEAD` is supported wherever `GET` is (RFC 9110 §9.1): an explicit `HEAD` route wins; otherwise the `GET` handler runs and the response carries its header section — including `Content-Length` / `Transfer-Encoding` — with no body and, for chunked framing, no last-chunk (RFC 9110 §9.3.2). Body suppression applies to every response to a `HEAD` request. |
| NR-10 | Repeated header field names combine into one comma-separated value in received order (RFC 9110 §5.2); a combined value that overflows storage → `431` (BR-7), never a silent replacement or drop. |

## 10. Observability requirements (OR)

| ID | Requirement |
|---|---|
| OR-1 | **Normative counter set**: `accepts`, `refusals_slot_full`, disconnects by reason `disconnect_{peer_closed, peer_reset, aborted, timeout_header, timeout_body, timeout_idle, timeout_write, timeout_drain, timeout_handler, server_closed}`, with `disconnect_timeout` their timeout total, `requests_completed`, `parse_errors`, and by class `parse_errors_{400, 413, 414, 417, 431, 501, 505}`, `backpressure_events` (TX ring full), `rx_window_throttle_events` (receive window held closed against the peer — routine under load), `rx_overflow_drops` (delivered bytes discarded because the RX assembly buffer was full — a desynced stream, and structurally unreachable while AR-6 holds), `slot_high_watermark`, `pending_dispatches` (handlers that returned `PENDING`), `pending_high_watermark`, `deferrals_refused` (a handler returned `PENDING` past the BR-12 cap and was answered `503`), `responses_refused` (a handler's response answered with the fixed `500` of BR-13 or BR-14), `bytes_in`, `bytes_out`, `post_init_heap_growth_bytes` (net heap growth since init, read from `pico_util::HeapWatermark` — a byte figure rather than an allocation count, because AR-1 permits one copied-body allocation per copied response, which a count would report on every such response). A fault counter is never folded into one that increments in normal operation: the two RX counters are separate because a drop buried in the throttle count cannot be observed, and `deferrals_refused` is separate from `pending_dispatches` for the same reason. `rx_window_throttle_events` covers both causes of a held-closed window — a response draining under backpressure and a parked deferral — because the counter names the effect on the peer, not the mechanism. |
| OR-2 | Counters exposed as JSON via `GET /stats`; reset via `POST /stats/reset` (benchmark firmware). |
| OR-3 | Configured limits (all BR/TR values) exposed via `GET /limits` so the benchmark driver auto-configures. |
| OR-4 | Build/profile identity (version, board, profile name, key lwipopts) exposed via `GET /info`. |

## 11. Measurement methodology

- **Percentiles, not averages** — nearest-rank P50/P95/P99 (+P99.9 for soak); ≥ 500
  samples for any p99 claim.
- **Warm-up** — discard an initial warm-up batch before sampling.
- **Units** — a KB is 1,024 B in every throughput target and figure here, as
  `bench.py` computes them.
- **RF environment** — record conditions and the default fifteen-request
  keep-alive HTTP bursts before and after the selected tests. Their min/median/
  max latency, sample count and errors are review evidence, never a verdict.
  A reviewer compares the same profile and build type with earlier runs and
  their RF-health observations. A persistent shift or a sudden step outside
  the history's usual spread warrants investigation; one noisy run is not an
  automatic failure. Keep the observation and its conditions. Idle ICMP is not
  a substitute for the HTTP burst.
- **Result format** — `bench.py` JSON artifacts (`results/bench-<timestamp>.json`)
  contain measured figures, numeric comparisons with the committed reference
  records, both RF-health summaries and functional/device-internal verdicts.
  Committed records stay unchanged. Review
  assesses trends and jumps; no numerical reference comparison changes exit
  status. Each record identifies firmware, profile, board, build type and time.
- **A/B protocol** — comparative runs execute on identical hardware, same
  session, interleaved.

## 12. Traceability matrix

Test IDs: `t1`–`t22` are on-device `bench.py` tests (see
[the benchmark suite](../examples/bench/README.md) for the suite
definition). Host-side tests are named by their CTest target. The targets rows
below cite are `pico_http_http_tests` and `pico_http_tcp_tests`, plus the
`pico_http_parser_fuzz` and `pico_http_service_fuzz` libFuzzer targets;
`pico_http_util_tests`, `pico_http_config_tests`, `pico_http_testing_tests` and
`pico_http_umbrella_tests` run in the same suite and are cited by no row.


Requirement IDs are cited in the code and tests that implement and exercise
them; review checks these citations (EN-02.1).

| Req | Test(s) | Pass criterion |
|---|---|---|
| AR-1 | t15, t7, t18; `pico_http_http_tests` — `SetBodyOverTheCapAllocatesNothing`, `ARefusedCopiedBodyIsFreedNotHeld`, `SpareBodyCapacityIsNotCarriedIntoTheSlot`, `ASentCopiedBodyIsNotHeldByTheSlot`, `ACopiedBodyStalledMidSendIsFreedWhenThePeerResets` | `post_init_heap_growth_bytes` is 0 across t15's static- and generator-body workload and bounded by `MAX_CONNECTIONS × MAX_BUFFERED_BODY` across the soak; t18 finds copied bodies served on every slot, repeated and at a smaller size, leave no growth once the slots are idle; on the host, a refused body allocates nothing, no slot holds more than `MAX_BUFFERED_BODY` in flight, and a sent or torn-down response leaves its slot holding nothing. |
| AR-2 | Review of the RX and TX paths; `t17` compares the body modes | RX copies each byte pbuf→assembly buffer, the head through the parser's scratch into its stored fields, and a body once more into the parser's body buffer; TX copies each body source once into the static ring and writes from it without `TCP_WRITE_FLAG_COPY`, buffered mode adding `set_body`'s copy (and the over-reserved trim). Review-only: no runtime counter exists |
| AR-3 | t5 on a Debug build | **Device-internal; mechanised.** p99 lock hold ≤ 500 µs under 8-connection load. A Release build compiles the sampler out, and `t5` reports AR-3 there as not measured (`ar3_measured` false) rather than claiming a pass |
| AR-4 | `pico_http_tcp_tests` | Stale-handle/ABA tests: generation mismatch → no-op |
| AR-5 | t11, t12, `pico_http_http_tests` | 256 KB bit-exact; slow reader isolated; resume-after-ack unit tests |
| AR-6 | t13, `pico_http_http_tests` | Throttle counter > 0 under stall, zero corruption; consume/window unit tests |
| AR-7 | `pico_http_tcp_tests` | Deadline tests across the 2³² ms wrap boundary |
| AR-8 | t1 | **Network-measured; review.** Record the latency distribution and assess modes or jumps against earlier matched runs and RF health; ~200 ms delayed-ACK behavior is a documented expectation to investigate, not a numerical gate. The Nagle-disable implementation remains required. |
| AR-9 | `pico_http_http_tests`, t19 | View lifetime across the deferred window; pipelining held; stale and duplicate `respond()` inert. t19 mechanises positive work and dispatches covering completions, no heavy errors/timeouts and no stranded entries. Its offered-load ratio, client-observed synchronous/deferred p99, improvement and 40 ms expectation are network-measured review evidence. |
| AR-10 | `lwipopts_assertions.hpp` in `lwip_tcp_transport.cpp`; the `consumer` gate cases in `house.toml`; case `lwipopts-required` at producer-configure, expecting `PICO_LWIPOPTS_TARGET must name the target` | Every §3.2 relation is a `static_assert`; the configure is refused with a diagnostic naming the variable; no target puts an `lwipopts.h` on the include path unless it is named |
| AR-11 | Compiler; `pico_http_http_tests` -- `ARequestViewCannotBecomeAStaticBody` | `set_body_static(request.body, ...)` does not compile: `cannot convert 'std::span<const unsigned char>' to 'pico_http::StaticBody'`. The test asserts the property the diagnostic rests on -- that neither a span nor a string view converts -- so a conversion added later fails there rather than silently restoring the trap. |
| AR-12 | the `consumer` gate cases `pico-w-refused` and `pico2-refused`, a consumer adding the library | On `pico_w` the configure fails saying `PICO_HTTP needs PICO_PLATFORM rp2350`; on `pico2`, saying `PICO_HTTP needs a board with the CYW43 radio` |
| PR-1 | t1 | **Network-measured; review.** t1 records latency percentiles (expectations p50 8 ms, p99 40 ms) and history comparisons. Review the same profile/build type, conditions and RF-health bursts for trends and jumps; no numerical miss fails the run. |
| PR-2 | t1 | **Mixed. Device-internal (mechanised):** receive callbacks through first send require `receive_within_1ms / receive_samples >= 0.99` at 1 ms, with max recorded. **Network-measured (review):** TTFB p50 expectation 6 ms and its history comparison; whole-callback/radio-handoff timings stay recorded. Review the matched history and conditions, without a network-speed verdict. |
| PR-3 | t5 | **Network-measured; review.** Record RPS at eight connections (expectation 250) and history comparison. Workload errors, RR-3 and Debug AR-3 remain mechanised. |
| PR-4 | t2 | **Network-measured; review.** Record serial connection p50 (expectation 25 ms) and device accepts under max-connection client load (expectation 150/s), alongside client rate/stalls and history. Accept counts use the board counter, but the measurement interval/load crosses Wi-Fi, so the rate is not device-internal. Failed exchanges or missing required stats remain mechanised failures. |
| PR-5 | t3 | **Network-measured; review.** At each swept size from 16 KiB, record 20 transfers and p10/median/min throughput (expectation p10 400 KB/s), plus history comparison. Review trends with conditions and RF health. Completion/truncation checks remain mechanised, with no speed gate. |
| PR-6 | t20 | **Network-measured; review.** Record aggregate and stall-free throughput at max connections (stall-free expectation 600 KB/s), stalls and history comparison. Complete transfers and zero refused lwIP allocations remain mechanised requirements. |
| PR-7 | t4 | **Network-measured; review.** Record buffered upload throughput (expectation 200 KB/s) and checksum/size observations at the accepted boundary, plus history comparison. No upload-speed verdict. |
| PR-8 | t11 | 256 KB download bit-exact |
| PR-9 | t20 | **Network-measured; review.** Record worst inter-read gap and comparison with history. The documented expectation is `2 * tcp_rto_ms / 1000 + 0.25` seconds; it and the recorded 10% reference comparison are informational, not limits. Complete transfers and allocation checks remain mechanised. |
| RR-1 | t8 | 0 slots leaked after 100 RSTs; immediate full-capacity accept |
| RR-2 | t21 | After 1,000 events rotating through a peer RST mid-request, a malformed request line (`400`), a header line over the cap (`431`) and a slowloris head, a fresh client's `/ping`, timed from connect to the full response, completes within 1 s while two slowloris heads still hold their slots, and the device reclaims those through the header timeout; every abuse request gets its expected status |
| RR-3 | t5 | 9th connection refused; 8 existing unaffected |
| RR-4 | t7, t21; `pico_http_http_tests` — `TheCountersAccountForEveryConnection` | t7 under mixed load and t21 under abuse, each recording its duration: no request errors (t7), no allocation lwIP refused from its heap or its pbuf, segment and `PBUF_ROM` pools (t7; `/lwip_stats` `err`), heap inside AR-1's bound (both) and flat (t7), every slot free once the load stops (`active_connections` back to the measuring connection within 20 s), and `accepts` plus the connections open at the reset (reported by `/stats/reset`) equal every disconnect reason plus `active_connections`, the timeout total its kinds and `parse_errors` its classes. The host case holds the same identities across every close path |
| RR-5 | t6 | Reuse rate ≥ 99 %, adjudicated by the run: every request after the first on a connection and within the keep-alive limit counts as one relying on reuse, so a server closing early lowers the rate rather than shortening the denominator |
| RR-6 | t22 | Every slot at once holds a backpressured 64 KiB download with 8 KiB of requests pipelined behind it, more than any profile's receive window; then every response is bit-exact and complete, `rx_window_throttle_events` is at least `MAX_CONNECTIONS`, `rx_overflow_drops` is 0, lwIP refused no allocation from its heap or its pbuf, segment and `PBUF_ROM` pools, and the device still serves. The pool's high-water mark since boot and `tcp.memerr`, which also counts writes refused for a full send queue, are recorded beside the verdict |
| BR-1..BR-11 | t10 (`Boundary matrix`); t5 (BR-6); `pico_http_http_tests` | Each boundary returns the status and close semantics its row in the boundary table states. Over the wire t10 reaches BR-1, BR-3, BR-5, BR-8, BR-9 and BR-11. BR-2 (single header line), BR-4 (head total), BR-7 (header storage overflow) and BR-10 (`Host`) are the parser and headers suites', which reach limits t10 cannot drive over the wire — BR-4 needs a `MAX_REQUEST_HEAD` small enough to trip before the header count does. BR-6 is the connection cap and belongs to t5 |
| BR-12 | `pico_http_http_tests` | Exhaustion → `503` + `Connection: close`; pool entry released on every teardown path |
| BR-13 | `pico_http_http_tests` — `OversizedResponseHeadYields500NotSilentClose`, `OversizedResponseHeadSendsOnlyTheFixed500` | A handler head over `HEAD_CAPACITY` yields exactly the fixed `500 Internal Server Error` head with `Content-Length: 0` and `Connection: close`, nothing of the handler's head or body follows, and the connection closes |
| BR-14 | `pico_http_http_tests` — `SetBodyOverTheCapAllocatesNothing`, `SetBodyAtTheCapIsCopiedAndClearsARefusal`, `CopiedBodyOverTheCapIsAnswered500`, `ARefusedCopiedBodyIsFreedNotHeld`, `CopiedBodyAtTheCapIsServed` | `set_body` over the cap leaves no buffer allocated and the response refused; a copied body one byte over `MAX_BUFFERED_BODY` yields exactly the fixed `500` head, closes and counts `responses_refused`, and the live heap afterwards does not hold a refused copied body; one of exactly `MAX_BUFFERED_BODY` is served whole through backpressure |
| TR-1 | t14; t9 | Device timer correctness remains mechanised. The network-observed closures in t9/t14 are robustness checks, not performance expectations. t14 adjudicates the ± 200 ms. t9 establishes the defense rather than the precision: it drips one header byte per second, so it cannot resolve better than that, and its band is 4,500–7,500 ms. What t9 answers is that the connection is closed at all, that `disconnect_timeout_header` counts it, and that a parallel client stays served meanwhile |
| TR-2 | `pico_http_http_tests` — `BodyProgressTimeoutFiresWithinItsBand`, `BodyProgressTimeoutClosesStalledUpload` | Open at 4,800 ms and closed by 5,200 ms without body progress, counted as `disconnect_timeout_body`. Host-side only: t14 measures the idle and header timeouts, the two observable from a client, and reaches neither this one nor TR-4 or TR-5 |
| TR-3 | t14, t6 | Device timer correctness and network-observed idle closure remain mechanised robustness checks. Idle close at configured value ± 200 ms |
| TR-4 | `pico_http_http_tests` — `WriteStallTimeoutFiresWithinItsBand`; t12 | Device deadline correctness and network-observed reclamation remain mechanised robustness checks. The host case holds the band: open at 9,800 ms and closed by 10,200 ms with no ACK progress, counted as `disconnect_timeout_write`. t12 establishes on device that the write stall fires and the parallel probe stays served; it checks `disconnect_timeout_write >= 1` and not when the timeout landed |
| TR-5 | `pico_http_http_tests` — `DrainCloseLingerFiresWithinItsBand`, `DrainCloseLingerTimeoutFires` | Open at 1,800 ms and closed by 2,200 ms with the response unacknowledged, counted as `disconnect_timeout_drain` and not `disconnect_server_closed`. Host-side only: t14 measures the idle and header timeouts and does not observe the linger |
| TR-6 | `pico_http_http_tests` | Deferred window bounded; expiry disconnects `timeout_handler` and makes the late `respond()` inert |
| NR-1 | `pico_http_http_tests`: `RejectsMalformedRequestLines`, `IgnoresExactlyOneLeadingCrlf`, `VersionHandling`, `SegmentationIndependenceAcrossEverySplit` | Malformed lines and extra leading CRLF fail; well-formed unsupported versions produce 505; all splits agree. |
| NR-2 | `pico_http_http_tests`: `KeepAliveByDefaultOnHttp11`, `Http10DefaultsToClose`, `Http10ClosesUnlessTheRequestAsksToPersist`, `HeaderContainsTokenMatchesWholeListElements`, `ResponseConnectionHeaderOverridesTheVersionDefault`, `ConnectionCloseHeaderClosesSocket`, `RequestCapClosesDespiteAHandlerSetKeepAliveHeader` | Version defaults, explicit close and request cap decide persistence. |
| NR-3 | `pico_http_http_tests`: `ContentLengthHygiene`, `ContentLengthPlusTransferEncodingIsBadRequest` | Duplicate, invalid, overflowing and ambiguous lengths fail. |
| NR-4 | `pico_http_http_tests`: `DecodesChunkedAcrossEverySplit`, `TransferEncodingOnHttp10IsBadRequest`, `RejectsControlBytesInExtensionsAndTrailers` | Chunked decoding agrees at every split; invalid framing and controls fail. |
| NR-5 | `pico_http_http_tests`: `Http10IgnoresContinueExpectation`, `Expect100ContinueEmittedBeforeBody`, `UnknownExpectationReturns417` | HTTP/1.0 ignores 100-continue and delivers the body; 1.1 sends 100 before body; other expectations give 417. |
| NR-6 | `pico_http_http_tests`: `RoutesGetRequestToHandler`, `ChunkedGeneratorResponseRoundTrips`, `ChunkedDowngradesToCloseDelimitedForHttp10` | 1.1 chunked output decodes exactly; HTTP/1.0 closes to delimit unknown length. |
| NR-7 | `pico_http_http_tests`: `Http11RequiresExactlyOneHost` | HTTP/1.1 missing or duplicate Host fails; HTTP/1.0 needs no Host. |
| NR-8 | `pico_http_http_tests`: `KnownUriWrongMethodReturns405WithAllow`, `UnknownRouteReturns404` | Known URI with wrong method gives 405 with Allow; unknown URI gives 404. |
| NR-9 | `pico_http_http_tests`: `HeadIsServedFromGetRouteWithoutBody`, `ExplicitHeadRouteWinsOverGetFallback`, `HeadOnChunkedRouteEmitsHeadersOnlyNoLastChunk` | HEAD suppresses every body and last-chunk; explicit HEAD wins. |
| NR-10 | `pico_http_http_tests`: `RepeatedHeaderFieldsCombineAsList`, `AddFailsWholeWhenCombinedValueOverflows`, `RepeatedHeaderValueOverflowIs431` | Repeated names combine in arrival order and overflow is refused. |
| OR-1 | `pico_http_http_tests` (the OR-1 counters cases, `PeerCloseAndTheTimeoutTotalAreCounted`, `LocalAbortIsCountedApartFromAPeerReset`, and the `parse_errors_*` and `disconnect_timeout_*` assertions of the boundary and timeout cases); t8; t13; t7 | Each counter's delta matches the injected events on the host; on device t8 checks `disconnect_peer_reset` under an RST burst, t13 `rx_window_throttle_events` and `rx_overflow_drops`, and t7 `post_init_heap_growth_bytes` |
| OR-2 | Review of `handle_stats` and `register_routes` in `examples/bench/main.cpp`; device bench stats deltas and resets; `consumer` cases `default` and `p8-rp2350` run `consumer_smoke` | GET /stats reports the counters and POST /stats/reset resets them. Consumer cases prove installed profile linkage, not endpoint schemas. |
| OR-3 | Device inspection of GET /limits; review of `handle_limits`; `consumer` cases `default` and `p8-rp2350` | JSON reports the compiled limits used by the boundary/timeout cases. The client can fall back when /limits is unavailable, so the complete field set needs review/device inspection. |
| OR-4 | Device inspection of GET /info; review of `handle_info`; `consumer` cases `default` and `p8-rp2350` | JSON identity matches the built version, board, profile and lwipopts. The bench bootstrap reads /info; complete identity fields need review/device inspection. |
