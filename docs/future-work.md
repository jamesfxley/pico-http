# Future work

Work deliberately not done, one entry per deferral, in the form `EN-03.7`
defines.

## FW-1 — Pico W (RP2040) validation

### What is absent

The library is refused on the Pico W (RP2040) at configure (AR-12), and no
board run exists on it. No RP2040 footprint budget is specified.

### Why it is not built

pico-http depends on pico-logging, whose lock-free assertions do not hold on the
RP2040's ARMv6-M core (pico-logging:FW-1), so the library cannot build for that
board until that entry is built. Validation then needs a Pico W on the bench.

### What would have to be true to build it

pico-logging:FW-1 is built. Then `PLATFORM rp2350` is dropped from this
library's bootstrap, the device suite runs on a Pico W through the repository's
device path with the measurements recorded in a run record, an RP2040 footprint
budget is specified, and the specification and README hardware rows are
updated.

### Version cost

Not breaking under `EN-06.1`: it admits one more board and removes no accepted
configuration.

### Affected repositories

pico-http; pico-logging:FW-1.

## FW-2 — Handler execution on the second core

### What is absent

All lwIP, transport and handler work runs in the cyw43 async-context path on one
core, so handler CPU time adds directly to network-path latency. The
deferred-completion API that admits moving it is built and specified (AR-9,
BR-12, TR-6): a handler returns `Disposition::PENDING` and resolves the request
later through `respond()`. The executor that would run that work on the second
core, `IHandlerExecutor`, does not exist.

### Why it is not built

The API and the executor were separated deliberately: the API decides whether
the lifetime and ownership rules are right, and the executor decides whether the
cross-core memory ordering is right. It is blocked on one decision. The pooled
response body vector would be written from the second core, and newlib `malloc`
is not multicore-safe without the SDK's lock wrapping. Either the pooled bodies
are reserved at init, which retires AR-1's bounded copied-body exception, or
the SDK's malloc locking is enabled and measured against AR-3's p99 lock-hold
budget.

### What would have to be true to build it

The pooled-bodies or malloc-locking decision is taken. Then lwIP, the transport,
the parser and the response writer stay on the first core; handlers run on the
second through static bounded SPSC queues with a multicore-FIFO doorbell; queue
overflow answers 503 through BR-12; and with no executor registered the service
behaves as it does today. Acceptance: AR-3's p99 lock hold stays within budget
under 8 connections, `/ping` latency stays within the reference record (FW-4)
while a heavy route saturates the second core, the bench suite (t1–t22) passes,
a 24-hour soak passes and post-init heap growth is zero.

### Version cost

Not breaking under `EN-06.1` for the executor seam, which is additive and inert
until an executor is registered. Breaking if pooled bodies are reserved at init,
because that retires a specified guarantee (AR-1).

### Affected repositories

pico-http.

## FW-3 — Server-side TLS

### What is absent

The server speaks cleartext HTTP only. There is no TLS listener and no
certificate handling.

### Why it is not built

It is sequenced after FW-2, because the handshake is the CPU-heavy work that
entry exists to move. Building TLS first would put the most expensive operation
the server performs directly in the network path, on the core that also runs
lwIP.

### What would have to be true to build it

FW-2 is built. Then TLS goes through mbedTLS (`pico_mbedtls`) and lwIP's
`altcp_tls`, server-only, behind the `ITcpTransport` seam. With a 4 KB maximum
fragment length a connection costs roughly 10–12 KB, against roughly 33 KB at
16 KB fragments, so the TLS connection count becomes its own knob over a static
`mbedtls_memory_buffer_alloc_init` pool. Crypto is ECDHE-ECDSA with a P-256
certificate in flash, session tickets, the RP2350's SHA-256 hardware and no
dependency on wall-clock time; mutual TLS stays out of scope. The RP2350 is the
only target, since FW-1 is unbuilt. Acceptance, made normative in the
specification revision that lands it: handshake p99 measured on device with 4
concurrent TLS connections, and plain-HTTP behaviour identical to before.

### Version cost

Breaking under `EN-06.1`. The NOCOPY transmit contract does not survive TLS,
because mbedTLS encrypts into its own buffer, so `send_credit` semantics change
and the zero-copy claim in the specification stops being true as written. The
resource budgets are replaced rather than extended.

### Affected repositories

pico-http.

## FW-4 — 0.1.0 bench reference records

### What is absent

No reference record from a bench run on 0.1.0 is committed. The records
`examples/bench/baselines/` is meant to hold are `pico2w-default.json`,
`pico2w-p8rp2350.json` and `pico2w-default-debug-t5.json`, and until they exist
the history comparison is recorded as unavailable. The 0.1.0 validation record
for the Pico 2 W is pending with them.

### Why it is not built

Each record is a run of the bench on a Pico 2 W with the matching profile and
build type, and none has been run and committed for this release.

### What would have to be true to build it

A bench run on a Pico 2 W for each listed profile and build type, committed
under `examples/bench/baselines/` with its conditions block and never replaced
to obtain a verdict.

### Version cost

Not breaking under `EN-06.1`: it adds records and changes no interface.

### Affected repositories

pico-http.
