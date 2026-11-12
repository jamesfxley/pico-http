# Benchmark baselines

This directory holds recorded `bench.py` measurements for review. None is
committed yet. A committed run will keep its timestamp, firmware
identity, profile, build type, board and conditions, and is never replaced to
obtain a verdict.

## Review

Network figures and numeric reference comparisons never decide the client's
exit status. Compare the same profile and build type with earlier recorded
runs, the documented expectations, and the RF-health bursts before and after
the selected tests. The driver records min/median/max latency, sample count and
errors for fifteen keep-alive pings in each burst. They never fail the run.
A persistent shift or sudden step beyond the history's usual spread is worth
investigating; one noisy observation is not a threshold failure. Keep its
conditions and assess whether RF-health drift explains it before attributing a
change to the server. Both board and client use Wi-Fi, so client wiring cannot
remove all radio interference.

Functional/robustness checks and device-internal timing retain mechanised
verdicts. A change to parser/service code or profile/lwipopts is a changed
condition to document and investigate at review, not a reason to replace the
recorded history. New JSON records numeric comparisons in `history_comparison`
and the two burst summaries in `rf_health`; no automatic performance verdict
is inferred from either.

## Run conditions

Board: Raspberry Pi Pico 2 W (RP2350), driven over Wi-Fi from the LAN by
`bench.py --host <ip>`. Idle ICMP to a Pico is bimodal from CYW43 power save and
looks like congestion, so RF health is checked with a short keep-alive HTTP
burst before and after a sweep rather than with ping; RF-health drift stays in the record for review alongside the sweep.

`CMAKE_BUILD_TYPE` is part of a run's conditions, not a detail: a Debug bench
build is materially slower than a Release one, so throughput and latency are
only comparable between baselines of the same build type. It also decides
whether lock-hold timing is measurable at all: the lwIP lock-hold sampler is `#ifndef
NDEBUG`, so a Release run reports `ar3_measured: false` and contributes no
lock-hold timing evidence. Each run records its build type, profile and board in a
`conditions` block.

## Baselines

No reference record on 0.1.0 is committed yet. The files this directory
is meant to hold:

| File | Profile | Build | Suite |
|---|---|---|---|
| `pico2w-default.json` | `default` | Release | full t1–t22 |
| `pico2w-p8rp2350.json` | P8-RP2350 (`-DPICO_HTTP_PROFILE=p8-rp2350`) | Release | full t1–t22 |
| `pico2w-default-debug-t5.json` | `default` | Debug | t5 only: the lock-hold timing record |

`history_comparison` selects the file by the run's recorded profile and build
type; until a file exists, the comparison is recorded as unavailable.

t4 has no pass field. t1 latency percentiles and t3 throughput percentiles
carry no speed verdict. t1 checks the internal receive-processing share; t3
checks complete, bit-exact transfers. t3 records nearest-rank p10, median and
minimum from 20 repetitions at every swept size. The throughput expectation
applies from 16 KiB. t2 and t20 record
expectations beside measurements.

## Reading a run

Run comparisons depend on the points below.

- Consecutive runs can differ substantially. Keep the RF-health observations
  before and after each sweep and assess link drift before attributing a change
  to the server.
- t2 can wait for PCB recycling if an earlier run leaves connections in
  TIME_WAIT, with no failures and normal percentiles. The reference
  `MEMP_NUM_TCP_PCB` is 12. Check `/lwip_stats` for `tcp_pcb.used == 1` and
  leave about two minutes between runs.
- RSSI is absent from run metadata. Throughput, latency and churn are reviewed
  with the recorded conditions.
- Opening `MAX_CONNECTIONS` connections at the same instant can lose one
  connection's acknowledgements. The device then sees nothing acked and
  retransmits from the first byte. When every device-side counter stays at
  zero, the loss is outside the service (UNCONFIRMED: no committed run
  record). Staggering the starts avoids it.

  The whole cost is one initial RTO, which is an `lwipopts` setting the
  application owns (AR-10). lwIP's `opt.h` documents 3000 ms as the traditional
  TCP value and 1 s (RFC 6298 §2.1) as the faster-recovery option. The
  reference configuration sets 1000 ms.

  An application supplying its own `lwipopts.h` and leaving lwIP's default
  pays 3000 ms. PR-9 records the gap beside its documented `2 × RTO +
  delivery` expectation for review with the history; it does not fail the run.

t20 records `stall_free_kbps` as bytes divided by the union of receiving
spans. Using one connection's span would incorrectly credit the others'
transfer time. Completion and allocation checks decide its verdict.

## Status

No 0.1.0 reference run is recorded. The run conditions above explain why
lock-hold timing needs a Debug build and why Debug rates cannot be compared
with Release.

History comparisons require matching recorded profile, build type and board.
The default Debug t5 record is selected for Debug; a missing or mismatched
reference is recorded as unavailable and never changes exit status.
