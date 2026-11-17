# pico-http

pico-http is a C++20 HTTP/1.1 server over raw lwIP for Pico 2 W (RP2350).
It depends on pico-logging and pico-util and uses House through the pinned
pico-tooling family. This guide covers local commands, layout and constraints.

The [pinned standard](../infra/house/standard/README.md) owns engineering
and C++ policy.

## Commands

`python3 dev.py check --ci` runs offline gates; `--list` names them.
`python3 dev.py build` configures, clean-builds and tests enabled trees.
`python3 dev.py build --tree host` selects the host tree.
`python3 dev.py gate consumer` builds relocated installed consumers.
`python3 dev.py format --check` and `python3 dev.py lint` run the tool groups.
The [family routes](../infra/agents/AGENTS.md) name the pinned guides.

Firmware needs `PICO_SDK_PATH` and an application lwIP target. The benchmark
needs non-empty credentials:

```sh
BUILD_ARGS='-DPICO_LWIPOPTS_TARGET=pico_http_lwipopts_fallback
-DPICO_HTTP_WIFI_SSID=placeholder-ssid
-DPICO_HTTP_WIFI_PASSWORD=placeholder-password' python3 dev.py check --ci
```

The matrix maps `HOUSE_MATRIX_PROFILE` to `PICO_HTTP_PROFILE`. Both named
profiles have host tests and footprint budgets in `pico.toml`.
`HOUSE_MATRIX_SANITIZE=address` or `thread` enables the sanitiser tree. Fuzz
targets run bounded inputs through CTest.
[Device benchmarks](../examples/bench/README.md) provide separate hardware
evidence.

## Layout

- `src/http/` holds the service, parser, messages and route table.
- `src/tcp/` holds the portable transport interface, slot table and deadline
  scheduler. `src/tcp/lwip/` supplies the firmware adapter.
- `src/util/` holds `TxRing`. `FixedString`, byte accessors and `HeapWatermark`
  belong to pico-util. Only the benchmark links `HeapWatermark`.
- `src/testing/` holds `FakeTcpTransport` for host tests of the HTTP layer.
- `src/lwipopts/` asserts application configuration in one translation unit.
  `src/lwipopts_reference/` supplies an explicitly selected reference
  configuration. Neither is linked automatically.
- `src/config/` carries sizes and profile definitions as INTERFACE usage
  requirements.
- `src/include/pico_http/pico_http.hpp` is the umbrella header, outside the
  module directories (CXX-13.1).
- `docs/` holds requirements, architecture and usage. Deliberately deferred
  work is in `docs/future-work.md` (EN-03.7).

## Conventions

### House policy

The [pinned engineering standard](../infra/house/standard/README.md) owns
common rules. The [family routes](../infra/agents/AGENTS.md) name the guides.

### Repo specifics

- Host targets take `-Werror` through `pico_http_strict`. Firmware applies it
  per owned source with `house_cxx_werror_sources`, because SDK INTERFACE
  libraries add vendor sources to linking targets. The build-log check catches
  unregistered owned sources.
- `docs/technical-specification.md` owns requirements under
  `AR/PR/RR/BR/TR/NR/OR`.
- The [allocation model](architecture.md#allocation) explains the copied-body
  exception (AR-1). A capacity failure is never a truncation;
  [boundary behaviour](architecture.md#boundary-behaviour) maps each one
  to its answer. A protocol tolerance, such as one leading CRLF, needs an RFC
  citation in the specification.
- The [custom-profile guide](usage-guide.md#a-custom-profile) describes profile
  and lwIP bindings. New size overrides need `static_assert` bindings so the
  layers cannot drift (AR-10). `tests/config/profile_binding_test.cpp` checks
  the window, MSS and pool bindings on the host.
- `BodyMode::STATIC_VIEW` must not be renamed `STATIC`: the cyw43 configport
  header defines that macro.
- The [baseline guide](../examples/bench/baselines/README.md) owns run names,
  conditions and comparison rules.
- Changes land as squash commits, with the title and body taken from the pull
  request.
- `ParseResult` keeps HTTP status codes as its error values. Zero is
  `INCOMPLETE`, so `is_error()` compares against `FIRST_ERROR_STATUS`.
  The service must send the encoded status. The [error model](architecture.md#errors)
  explains why this differs from `pico_util::Result`.
