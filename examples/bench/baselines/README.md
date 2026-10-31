# Benchmark baselines

This directory holds recorded `bench.py` measurements for review. None is
committed yet (FW-4). A committed run will keep its timestamp, firmware
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
whether AR-3 is measurable at all — the lwIP lock-hold sampler is `#ifndef
NDEBUG`, so a Release run reports `ar3_measured: false` and contributes no
AR-3 evidence. Each run records its build type, profile and board in a
`conditions` block.

## Baselines

No reference record on 0.1.0 is committed yet (FW-4). The files this directory
is meant to hold:

| File | Profile | Build | Suite |
|---|---|---|---|
| `pico2w-default.json` | `default` | Release | full t1–t22 |
| `pico2w-p8rp2350.json` | P8-RP2350 (`-DPICO_HTTP_PROFILE=p8-rp2350`) | Release | full t1–t22 |
| `pico2w-default-debug-t5.json` | `default` | Debug | t5 only: the AR-3 record |

`history_comparison` selects the file by the run's recorded profile and build
type; until a file exists, the comparison is recorded as unavailable.

t4 carries no `pass` field and t1's latency percentiles carry no verdict: they
record numbers. Comparisons are review evidence, without a numerical failure
rule. t1's `pass` is PR-2's internal share; t3's is PR-5, taken per size from
16 KiB on the p10 of 20 reps by nearest rank (`kbps_p10`, with `kbps_median` and
`kbps_min` beside it, `judged_percentile` naming the percentile). t2 and t20
carry their documented expectations beside their measurements.


## Reading a run

Four properties of this instrument decide whether a difference means anything.
They are conditions, not results, and they apply to every run.

- **Link variance is large enough to fake a regression.** Identical
  back-to-back runs with no code change between them can differ severalfold in
  t1 latency and request rate, and two consecutive full runs can trade places
  on t3. Bracket a sweep with a short keep-alive burst before and after, and
  discard a run whose brackets show the link degraded. Runs retain their
  RF-health observations for review.

- **t2 needs a drained TIME_WAIT pool to be meaningful.** The reference
  `MEMP_NUM_TCP_PCB` is 12, so a run that starts while an earlier run's PCBs are
  still in TIME_WAIT is throttled on PCB recycling, with no failures and every
  latency percentile normal. Check `/lwip_stats` for `tcp_pcb.used == 1` before
  trusting a t2 number, and leave ~2 minutes between runs.

- **RSSI is not recorded in the run metadata.** RF-sensitive figures —
  throughput, latency and t2 churn — are review evidence, not numerical limits.

- **A synchronised burst costs one connection a retransmission, and the
  initial RTO sets what that costs.** Opening `MAX_CONNECTIONS` connections at
  the same instant can lose one connection's acknowledgements; the device then
  sees nothing acked and retransmits from the first byte, while every
  device-side counter — lwIP `drop`, `memerr`, `err`, the pools, the service's
  own timeout and refusal counts — stays at zero, so this is the link under a
  burst rather than anything the service did. Staggering the starts avoids it.

  The whole cost is one initial RTO, which is an `lwipopts` setting the
  application owns (AR-10). lwIP defaults to 3000 ms because it is a portable
  stack; the reference configuration sets RFC 6298's 1000 ms.

  An application supplying its own `lwipopts.h` and leaving lwIP's default
  pays 3000 ms. PR-9 records the gap beside its documented `2 × RTO +
  delivery` expectation for review with the history; it does not fail the run.

  t20 takes its throughput verdict on `stall_free_kbps`: the bytes over the
  union of the connections' receiving spans, which is the interval the device
  was delivering to somebody. The union matters — dividing by any one
  connection's active time credits the set with time it spent serving the
  others and reads as several times the radio's ceiling.

## Status

No 0.1.0 run is recorded. AR-3's on-device record is the Debug t5 run above;
`ar3_measured` is false on a Release run, because the lwIP lock-hold sampler is
`#ifndef NDEBUG`. A Debug build is materially slower, so its rates are not
comparable with Release runs.

History comparisons require matching recorded profile, build type and board.
The default Debug t5 record is selected for Debug; a missing or mismatched
reference is recorded as unavailable and never changes exit status.
