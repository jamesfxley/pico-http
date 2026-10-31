# pico-http

C++20 HTTP/1.1 server library for the Raspberry Pi Pico 2 W (RP2350), built on
the lwIP raw API with a statically allocated core. Part of the pico-* family:
it depends on pico-logging and pico-util, uses House build helpers through its pinned pico-tooling family.

Orientation for anyone working in this repository: how to build and gate it,
how it is laid out, and the repository's invariants and traps.

Engineering standards, cited by rule ID (`EN-`/`CXX-`):
<https://github.com/jamesfxley/engineering-standard>

## Commands

`python3 dev.py check --ci` runs the offline gates; `--list` names them.
`python3 dev.py build` configures, clean-builds and tests enabled trees.
`python3 dev.py build --tree host` selects only the host tree, and
`python3 dev.py gate consumer` builds relocated installed consumers.
`python3 dev.py format --check` and `python3 dev.py lint` run the tool groups.
The pinned guides are routed through `infra/agents/AGENTS.md`.

Firmware needs PICO_SDK_PATH and an explicit application lwIP target.
The benchmark needs non-empty credentials to retain its connection path:

```sh
BUILD_ARGS='-DPICO_LWIPOPTS_TARGET=pico_http_lwipopts_fallback
-DPICO_HTTP_WIFI_SSID=placeholder-ssid
-DPICO_HTTP_WIFI_PASSWORD=placeholder-password' python3 dev.py check --ci
```

The profile matrix maps HOUSE_MATRIX_PROFILE to PICO_HTTP_PROFILE. Both
`default` and `p8-rp2350` are host-tested and have firmware footprint budgets
in pico.toml. HOUSE_MATRIX_SANITIZE=ON enables the sanitizer tree; fuzz targets
run bounded inputs through CTest. Device runs and sustained benchmark tests are
separate hardware evidence; see examples/bench/README.md.

## Layout

- `src/http/` — `HttpService`, incremental `HttpParser` (header-only detail),
  message types, route table.
- `src/tcp/` — `ITcpTransport` seam (host-safe) + `LwipTcpTransport`
  (firmware-only); slot table, connection ids, timeout scheduler in `detail/`.
- `src/util/` — header-only `TxRing`, and nothing else. `FixedString`, the
  byte accessors and `HeapWatermark` all live in pico-util; only the benchmark
  example links the last of them.
- `src/testing/` — `FakeTcpTransport`; all HTTP-layer behavior is host-tested
  against it. Only the lwIP glue needs hardware.
- `src/lwipopts/` — what this library asserts of the application's lwIP
  configuration (`lwipopts_assertions.hpp`, included by exactly one translation
  unit). `src/lwipopts_reference/` holds a configuration meeting it. Neither is
  in the automatic link graph: the application names its own in
  `PICO_LWIPOPTS_TARGET`.
- `src/config/` — build-wide sizing policy and the named profiles. Header-only,
  and the target carrying the profile compile definitions as INTERFACE usage
  requirements.
- `src/include/pico_http/pico_http.hpp` — the umbrella, belonging to no module
  (`CXX-13.1`).
- `docs/` — normative spec (requirement IDs `AR/PR/RR/BR/TR/NR/OR`),
  architecture and usage guide. Work deliberately not done is in
  `docs/future-work.md` (`EN-03.7`).

## Conventions

### House policy

The [pinned engineering standard](../infra/house/standard/README.md) owns the
common rules. The [family routes](../infra/agents/AGENTS.md) name the guides.

### Repo specifics

- **`-Werror` follows the toolchain.** The host tree links no SDK sources, so
  it sits on `pico_http_strict`; a firmware target cannot take it, because the
  SDK compiles its own sources into whichever target links its INTERFACE
  libraries, so the four cross-compiled sources take it per source via
  `house_cxx_werror_sources`. The CI build-log grep is the backstop for a
  source added but never registered.

- `docs/technical-specification.md` is normative. Requirement IDs are
  `AR/PR/RR/BR/TR/NR/OR`.
- **Zero post-init heap is AR-1.** A capacity failure is never a truncation;
  `docs/architecture.md` (Boundary behavior) maps each one to its answer. Any
  tolerance (e.g. one leading CRLF) needs an RFC citation in the spec.
- **Profiles** come from `PICO_HTTP_ACTIVE_PROFILE` and the MSS-factor macros.
  The per-profile lwIP values they pair with are in
  `src/lwipopts_reference/include/lwipopts_reference.h`. AR-10's requirements on
  an application's lwipopts are asserted in
  `src/lwipopts/include/pico_http/lwipopts_assertions.hpp`, except the mirrors
  of `TCP_WND`, `TCP_MSS` and `TCP_SND_BUF`, which `src/tcp/lwip/lwip_tcp_transport.cpp`
  asserts; `tests/config/profile_binding_test.cpp` checks the window, MSS and pool
  on the host.
  A new knob needs a `static_assert` binding it to lwipopts so the two cannot
  drift.
- **Naming trap.** `BodyMode::STATIC_VIEW` must not be renamed `STATIC`: the
  cyw43 configport header defines `STATIC` as a macro in firmware builds.
- **Baselines.** Which run is promoted, its file name, its run conditions and
  what requires a fresh run are set out in `examples/bench/baselines/README.md`.
- The repository is squash-only, with the squash title and body taken from the
  pull request.

- **`ParseResult` is this library's taxonomy and stays as it is.** Its zero is
  `INCOMPLETE` — a state, not success — and its non-zero values *are* the HTTP
  status the service must send, which is why `is_error()` discriminates against
  `FIRST_ERROR_STATUS` rather than against zero. It is deliberately not a
  `pico_util::Result` error type: that convention needs `E{}` to mean success
  and would destroy the status-code-is-the-value property. The family rule it
  sits under: a library owns the taxonomy of the failures it can produce, and
  re-exports no dependency's. `docs/architecture.md` has the whole policy.
