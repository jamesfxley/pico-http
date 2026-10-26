# pico-http benchmark example

Firmware that exercises pico-http's static core (`HttpService` over
`LwipTcpTransport`, see [../../docs/architecture.md](../../docs/architecture.md))
on real hardware, plus a host-side Python driver (`bench.py`) that measures
latency, throughput, upload behaviour, concurrency scaling, keep-alive
semantics, and the spec's robustness gates (RST/abort teardown, slowloris,
boundary enforcement, flow control, timeout precision).

## Build

The example is firmware-only and builds as part of the pico-http firmware
build (it is skipped on host builds). Wi-Fi credentials are compile-time:

```sh
# From the repo root; use real credentials for a device run.
BUILD_ARGS='-DPICO_LWIPOPTS_TARGET=pico_http_lwipopts_fallback
-DPICO_HTTP_WIFI_SSID=my-ssid
-DPICO_HTTP_WIFI_PASSWORD=my-password' python3 dev.py build --tree firmware
```

The image is `build-firmware/examples/bench/pico_http_bench.uf2`.
`PICO_HTTP_ENABLE_EXAMPLES=OFF` disables it. Credentials are explicit configure
arguments; the build does not read a credentials file automatically.

## Flash and console

Follow [the pinned family device guide](../../infra/guides/device.md) for
flashing and console access. A device action requires specific authorization.
Run the benchmark only after the image joins Wi-Fi and answers `/ping`.

## TCP path for transport cases

OR-1's peer-reset count (t8) and TR-4's write-stall case (t12) require a
packet-level TCP path between client and board. Use Docker Engine on Linux,
or Docker Engine in WSL NAT mode with bridge networking. Docker Desktop is
not an accepted client path for these cases: its backend opens its own host
TCP connections, so client resets and receive-window stalls do not exercise
the board's corresponding transport paths. See [Docker's networking description](https://docs.docker.com/desktop/features/networking/#how-containers-connect-to-the-internet)
and [Engine bridge networking](https://docs.docker.com/engine/network/drivers/bridge/).

## Performance review

Correctness is mechanised. Client-observed speed and send gaps are review
evidence: their documented numbers are expectations, not failure limits.
Device-internal receive processing and Debug lock timing keep their bounds.
Protocol timeout, recovery, integrity, accounting and allocation checks keep
failing the run when they fail.

The driver brackets selected tests with fifteen keep-alive `/ping` requests.
Their min/median/max latency, samples and errors print and appear as `rf_health`
in JSON. Burst errors never fail the run. `history_comparison` records measured
figures beside the committed reference record and their percentage change, without
performance verdicts. A burst of fifteen is not a qualified p99 sample.

Review the same profile and build type against earlier recorded runs, with
conditions and both RF-health bursts. Investigate a persistent trend or sudden
step beyond the history's usual spread, especially with stable RF-health
figures. A single miss is a recorded observation. Both client and board use
Wi-Fi; client wiring alone cannot remove all radio interference. Committed
records stay unchanged once committed.

## Run the benchmark

```sh
python3 bench.py --host 192.168.1.42
python3 bench.py --host 192.168.1.42 --tests t1,t3,t6
python3 bench.py --host 192.168.1.42 --tests t7 --duration 300   # soak
```

Results print as a table and are saved to `results/bench-<timestamp>.json`
(the `results/` directory is git-ignored; reference runs are promoted to the
committed [baselines/](baselines/)). All tests run by default except **t7**
(soak — opt in and set the length with `--duration`), **t14** (timeout
precision — slow), **t17** (body-mode comparison), **t18** (copied-body heap
mechanism — long and heap-heavy) and **t21** (abuse run — 1,000 connections,
long). Tests:

- **t1** latency — sequential `GET /ping` over keep-alive (p50/p90/p99, rps,
  TTFB split; spec PR-1/PR-2); mechanises PR-2's internal share from the device's
  receive-callback sample
- **t2** connection churn — the accept rate with `max_connections` workers
  (≥ 150/s) and the serial connect p50 (≤ 25 ms) (PR-4)
- **t3** download throughput vs size — bit-exact up to `payload_max_bytes`, no
  ceiling; records PR-5 at each size from 16 KiB across 20 reps: p10, median and slowest
  throughput beside the 400 KB/s expectation; completion remains mechanised
- **t4** upload throughput + integrity — finds `max_reliable_upload` (explicit 413 boundary)
- **t5** concurrency scaling — 1..N+1 parallel workers; verifies the
  `max_connections` cap and (debug builds) the AR-3 lock-hold bound
- **t6** keep-alive semantics — request cap + `Connection: close` on the last
- **t7** soak (opt-in, `--duration`) — mixed load, heap/error drift, then every
  slot free, the counters balanced and no lwIP allocation refused (RR-4)
- **t8** RST robustness — 100 hard resets (`SO_LINGER(0)`), then confirms every
  connection slot is immediately reusable (RR-1; the RST teardown check)
- **t9** slowloris — dripped header bytes; the connection closes at the fixed
  TR-1 window while a parallel client stays served
- **t10** boundary matrix — 414/431/413/400/417/501/505 responses, chunked
  `Transfer-Encoding` decode (extensions, trailers, oversize), HEAD-of-GET,
  leading-CRLF tolerance, and `Expect` semantics. It reaches BR-1, BR-3, BR-5,
  BR-8, BR-9 and BR-11 and the NR set; BR-2, BR-4, BR-7 and BR-10 are the host
  parser and headers suites', and BR-6 is t5's
- **t11** backpressure download — 256 KB generator body, bit-exact through a
  4×MSS TX ring (PR-8, AR-5)
- **t12** slow-reader isolation — a stalled reader is reclaimed by TR-4 while
  a parallel `/ping` probe stays unaffected
- **t13** RX flow control — pipelined bytes held with the window closed
  (`rx_window_throttle_events > 0`), zero corruption (AR-6)
- **t14** timeout precision (opt-in) — configured vs actual closure for the
  idle and header timeouts, the two cleanly observable from a client
- **t15** static footprint / heap growth — asserts
  `post_init_heap_growth_bytes == 0` after the suite (AR-1)
- **t16** chunked download — `GET /stream/<n>` returns `Transfer-Encoding:
  chunked` with no `Content-Length`, bit-exact across sizes, keep-alive survives
  (NR-6)
- **t17** body-mode comparison (opt-in) — BUFFERED vs STATIC_VIEW throughput and
  latency for an identical body via `/bench/buffered/<n>` and `/bench/static/<n>`
  Default size 2048 (both profiles); run
  `--tests t17 --sizes 8192` on the `p8-rp2350` profile for the 8 KiB case. A
  buffered body over `MAX_BUFFERED_BODY` is reported as skipped (413), not failed
- **t18** copied-body heap mechanism (opt-in; long and heap-heavy) — asserts the
  AR-1 copied-body model as pass/fail gates: static/generator paths add ~zero
  over thousands of requests; copied bodies served on every slot at once, then
  repeated and at a smaller size on one connection, leave ~zero growth once the
  slots are idle
- **t19** deferred vs synchronous dispatch under load — a heavy handler run
  deferred must not push `/ping` p99 past the PR-1 ceiling the synchronous run
  breaches (AR-9)
- **t20** aggregate download — `max_connections` parallel pulls of one body,
  recorded for review (PR-6). Reports both the wall-clock aggregate and
  `stall_free_kbps`: starting every connection at once
  loses one connection's acks in the burst, and the device's retransmission
  after its RTO dominates a wall-clock figure without saying anything about the
  service. `worst_gap_s` is recorded beside `2 x tcp_rto_ms` plus delivery (PR-9),
  so a reviewer can compare burst behavior with the history; any lwIP
  allocation refused during the run fails it
- **t21** abuse run — 1,000 events rotating a peer RST mid-request, a malformed
  request line, an over-long header line and a slowloris head; then a fresh
  `/ping` within 1 s while two slowloris heads hold their slots and are reclaimed
  by the header timeout (RR-2), and every slot free, the heap within bound and
  the counters balanced (RR-4)
- **t22** pool pressure — every slot holds a backpressured download with 8 KiB
  of requests pipelined behind it, filling every receive window at once, then
  is read out bit-exact, with a throttle per slot, no RX overflow drops and no
  lwIP allocation refused (RR-6); reports
  the pbuf pool's high-water mark since boot

History comparisons require matching recorded profile, build type and board.
The default Debug t5 record is selected for Debug; a missing or mismatched
reference is recorded as unavailable and never changes exit status.
