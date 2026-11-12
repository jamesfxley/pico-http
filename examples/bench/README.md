# pico-http benchmark example

The benchmark firmware serves routes over `HttpService` and
`LwipTcpTransport`. The host driver `bench.py` checks protocol behaviour,
recovery, flow control and allocation, and records network performance for
review. [Architecture](../../docs/architecture.md) describes the server.

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
flashing and console access. A device action requires specific authorisation.
Run the benchmark only after the image joins Wi-Fi and answers `/ping`.

## TCP path for transport cases

The peer-reset count (t8) and write-stall case (t12) require a
packet-level TCP path between client and board. Use Docker Engine on Linux,
or Docker Engine in WSL NAT mode with bridge networking. Docker Desktop is
not an accepted client path for these cases: its backend opens its own host
TCP connections, so client resets and receive-window stalls do not exercise
the board's corresponding transport paths. See [Docker's networking description](https://docs.docker.com/desktop/features/networking/#how-containers-connect-to-the-internet)
and [Engine bridge networking](https://docs.docker.com/engine/network/drivers/bridge/).

## Performance review

Protocol, recovery, integrity, accounting, allocation and device-internal
timing checks decide pass or fail. Client-observed speed and send gaps are
recorded for review against documented expectations.

The [baseline guide](baselines/README.md) owns run conditions, RF-health
bursts, history selection and performance review. Committed records remain
unchanged.

## Run the benchmark

```sh
python3 bench.py --host 192.168.1.42
python3 bench.py --host 192.168.1.42 --tests t1,t3,t6
python3 bench.py --host 192.168.1.42 --tests t7 --duration 300   # soak
```

Results print as a table and are saved to `results/bench-<timestamp>.json` (the
`results/` directory is git-ignored; reference runs are promoted to
[baselines/](baselines/), where none is committed yet). All tests run by
default except t7 (soak: opt in and set the length with `--duration`),
t14 (timeout precision: slow), t17 (body-mode comparison), t18
(copied-body heap mechanism: long and heap-heavy) and t21 (abuse run: 1,000 connections, long). Tests:

| Test | Checks and recorded results |
|---|---|
| t1 | Keep-alive `/ping` latency, request rate and TTFB; checks the device's internal receive-processing share. |
| t2 | Parallel connection churn and serial connect latency; records expectations of 150 accepts/s and 25 ms p50. |
| t3 | Bit-exact downloads across swept sizes. Records p10, median and minimum throughput from 20 repetitions, with a 400 KB/s expectation from 16 KiB. |
| t4 | Upload throughput and integrity; finds `max_reliable_upload` and the explicit 413 boundary. |
| t5 | One through `max_connections + 1` workers; checks the connection cap and Debug lock-hold timing. |
| t6 | Checks the Keep-Alive header, the request cap and the reuse rate; records `Connection: close` on the final response. |
| t7 | Opt-in mixed-load soak; checks heap growth, allocation errors, released slots and balanced counters. |
| t8 | One hundred peer resets with `SO_LINGER(0)`, followed by full slot reuse. |
| t9 | Dripped headers; checks fixed header timeout and concurrent service of a normal client. |
| t10 | Error statuses, chunked decode, HEAD fallback, leading CRLF and Expect handling. Request-line, header-count, body, syntax, transfer-coding and HTTP-version limits run on the wire. Host tests cover header-line, head-total, header-storage and Host limits; t5 covers the connection cap. |
| t11 | Bit-exact 256 KB generated download through a 4×MSS TX ring. |
| t12 | Reclaims a stalled reader through the write timeout while a parallel `/ping` remains served. |
| t13 | Pipelined input held behind a closed receive window; checks throttle events and zero corruption. |
| t14 | Opt-in header and idle timeout precision. |
| t15 | Checks zero post-init heap growth after the static/generator workload. |
| t16 | Bit-exact chunked `/stream/<n>` output without Content-Length, preserving keep-alive. |
| t17 | Opt-in buffered/static comparison through `/bench/buffered/<n>` and `/bench/static/<n>`. Default size is 2048 for both profiles. Use `--sizes 8192` with `p8-rp2350` for 8 KiB. Buffered bodies above the cap are skipped with 413. |
| t18 | Opt-in copied-body allocation checks. Static/generator requests leave approximately zero growth. Copies across all slots, repeated and at smaller sizes, leave approximately zero growth after slots become idle. |
| t19 | Checks deferred work, dispatch coverage, heavy-work errors, handler timeouts and stranded pool entries. Latency, improvement and offered-load ratio are review evidence. |
| t20 | Parallel downloads; records wall-clock and `stall_free_kbps` throughput, worst receive gap and the `2 × tcp_rto_ms` plus delivery expectation. Completion and lwIP allocation checks decide the verdict. |
| t21 | One thousand reset, malformed-line, overlong-header and slowloris events. A fresh `/ping` completes within 1 s while two slowloris slots remain, then timeout reclaims them. Checks slots, heap and counters. |
| t22 | Backpressured downloads across all slots with 8 KiB pipelined input behind each. Checks complete bit-exact output, throttling, zero overflow and no failed lwIP allocations. Records the pbuf high-water mark. |

History comparisons require matching recorded profile, build type and board.
The default Debug t5 record is selected for Debug; a missing or mismatched
reference is recorded as unavailable and never changes exit status.
