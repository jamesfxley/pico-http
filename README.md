# pico-http

[![CI](https://github.com/jamesfxley/pico-http/actions/workflows/ci.yml/badge.svg)](https://github.com/jamesfxley/pico-http/actions/workflows/ci.yml)

pico-http is a C++20 HTTP server for Raspberry Pi Pico 2 W. Fixed connection
slots, parser buffers and a transmit ring bound its core storage. Boundary
failures return HTTP errors; the connection cap rejects excess connections at
TCP level. The [allocation model](docs/architecture.md#allocation) describes
the bounded copied-body exception.

All public symbols live in `namespace pico_http` with headers under the
`pico_http/` prefix.

## Quick start

```cmake
include(FetchContent)
FetchContent_Declare(pico_http
    GIT_REPOSITORY https://github.com/jamesfxley/pico-http.git
    GIT_TAG v0.1.0)  # pin the release you build against
set(PICO_HTTP_BUILD_FIRMWARE ON CACHE BOOL "")
set(PICO_LWIPOPTS_TARGET app_lwipopts)  # the INTERFACE target carrying your lwipopts.h
FetchContent_MakeAvailable(pico_http)

target_link_libraries(app PRIVATE pico_http::core)
```

Your application provides lwIP’s `lwipopts.h`. `PICO_LWIPOPTS_TARGET` must
name the INTERFACE target carrying it; configuration fails without that
target. There is no default because the SDK compiles lwIP into each linking
target, and differing configurations would produce incompatible copies.
`pico_http::lwipopts_fallback` is a reference configuration to copy or select
explicitly.

<!-- example: readme_quick_start -->
```cpp
#include <pico_http/pico_http.hpp>

auto server = pico_http::make_http_service();  // firmware builds
const bool routed = server->add_route(
    "GET", "/status",
    [](const pico_http::HTTPRequestView&, pico_http::HTTPResponse& res) {
        res.set_body("ok");
        return pico_http::Disposition::COMPLETE;
    });
if (!routed) { /* the method or pattern is too long */ }
if (!server->start(80)) { /* port bind failed */ }
```

Responses use copied, static or generated bodies. A generator with a known
length uses `Content-Length`; one without a length uses chunked framing for
HTTP/1.1 or close-delimited framing for HTTP/1.0. `HEAD` uses a registered
`GET` handler unless a `HEAD` handler is registered. The [usage
guide](docs/usage-guide.md) covers each mode.

Host code injects the fake transport to run without hardware:

<!-- example: readme_host_fake -->
```cpp
pico_http::HttpService server(
    std::make_unique<pico_http::testing::FakeTcpTransport>());
server.start(80);
```

## Layout

| Target | Contents |
|---|---|
| `pico_http::http` | `HttpService` (routing, incremental parsing, streaming responses, keep-alive, timeouts, statistics) and the `HTTPRequestView`/`HTTPResponse` message types |
| `pico_http::tcp` | The host-safe `ITcpTransport` interface, connection IDs, slot table, timeout scheduler |
| `pico_http::tcp_lwip` | `LwipTcpTransport`, the lwIP raw-API implementation (firmware builds) |
| `pico_http::config` | The sizing profile, boundary limits and slot count. Carries the profile selection as a usage requirement, so a consumer is compiled against the same layout it links |
| `pico_http::util` | `TxRing`, the header-only transmit ring. `FixedString` and the byte accessors are pico-util's |
| `pico_http::lwipopts_assertions` | What this library requires of the application's `lwipopts.h`. Assertions only; it defines no lwIP option |
| `pico_http::lwipopts_fallback` | A reference configuration meeting them, to copy or to name in `PICO_LWIPOPTS_TARGET`. Outside the automatic link graph |
| `pico_http::testing` | `FakeTcpTransport` and `ServiceHarness`: drive the HTTP layer from host tests (deliver bytes, ack sends, inject RSTs, advance time). Exported with the package from a host build; a firmware build exports `tcp_lwip` instead |
| `pico_http::core` | Umbrella INTERFACE target linking the library targets above |

Product dependency pins for pico-logging and pico-util live in CMakeLists.txt.
The `infra` submodule supplies the pinned family and House build support.
Source overrides are explicit; sibling checkouts are not discovered.

## Building and tests

Initialise the pinned dependencies with `git submodule update --init
--recursive`. The family supplies build helpers, gates and the devcontainer.
`CMakeLists.txt` declares product revisions.

```sh
python3 dev.py check --ci
python3 dev.py build --tree host
python3 dev.py gate consumer
```

[docs/orientation.md](docs/orientation.md) explains the firmware inputs and
profile matrix. [The family build guide](infra/docs/build.md) covers shared
commands. Device benchmarks are separate from build and static-footprint checks.

Host tests are configured only when pico-http is the top-level project and
not cross-compiling.

`tests/device/` checks TCP backpressure, receive-window reopening, connection
capacity and deferred completion over lwIP loopback. It bypasses CYW43 Wi-Fi
and needs no network join.
Select its lwIP configuration for the whole firmware build:

```sh
HOUSE_MATRIX_PROFILE=default \
HOUSE_MATRIX_LWIPOPTS=pico_http_device_lwipopts \
HOUSE_MATRIX_SSID=placeholder-ssid \
HOUSE_MATRIX_PASSWORD=placeholder-password \
python3 dev.py build --tree firmware
```

The placeholder credentials satisfy the separately built benchmark's configure
contract. Run `build-firmware/tests/device/pico_http_device_tests.uf2` using the
[device-test procedure](infra/guides/device.md#device-tests). The fallback CI
configuration builds the image but makes its cases report FAIL for missing
loopback options. The `pico_http::device_lwipopts` export is internal link
support; it is not an installed public module target.

## Sizing profiles

Every static allocation is sized from a compile-time profile. Two are shipped;
the `TCP_WND` and `PBUF_POOL_SIZE` columns are the reference lwipopts' values:

| Profile | Buffered body | TX ring | `TCP_WND` | `PBUF_POOL_SIZE` |
|---|---|---|---|---|
| `default` | 2 KiB | 4×MSS | 2×MSS | 24 |
| `p8-rp2350` | 8 KiB | 4×MSS | 4×MSS | 40 |

Select one whole-build at configure time:

```sh
cmake -B build-firmware -DPICO_HTTP_PROFILE=p8-rp2350 ...
```

The profile selection propagates through `pico_http::config` to consumers,
transport sizes and lwIP options. Compile-time assertions check the bindings.
The [custom-profile guide](docs/usage-guide.md#a-custom-profile) covers
replacement profiles. The header caps are set independently with
`PICO_HTTP_MAX_HEADER_NAME` and `PICO_HTTP_MAX_HEADER_VALUE`
([specification §3.5](docs/technical-specification.md#35-header-storage-knob)).

## On-device records

The device suite covers request framing, timeouts, resets and allocation on
Pico 2 W. The [baseline guide](examples/bench/baselines/README.md) explains
which results decide a verdict and which are recorded for review. Reference
records are deferred; see [future work](docs/future-work.md).

Pico W (RP2040), second-core handler execution and server-side TLS are outside
the supported surface. Their constraints are in [future work](docs/future-work.md).

## Documentation

- [Documentation index](docs/README.md): what each document is for
- [Orientation](docs/orientation.md): commands, conventions and traps, before
  a first change
- [Contributing](CONTRIBUTING.md): how a change is proposed, and what the gates
  ask of it
- [Technical specification](docs/technical-specification.md): normative;
  requirement IDs `AR/PR/RR/BR/TR/NR/OR`, their pass criteria and the
  traceability matrix
- [Architecture](docs/architecture.md): how the stack works: connection
  slots, RX/TX paths, backpressure, timeouts, observability
- [Usage guide](docs/usage-guide.md): routes, body modes,
  configuration, host testing
- [Benchmark suite](examples/bench/README.md): on-device endpoint
  contract and `bench.py` results
- [Standing deviations](docs/deviations.md): scope-wide rule suppressions and
  their review dates
- [Releases](https://github.com/jamesfxley/pico-http/releases): what changed per
  release, and what to change in your code when an API broke

## Releases

Public API compatibility and release notes follow [EN-06.1 and
EN-06.3](infra/house/standard/engineering/06-release-and-versioning.md).
[Releases](https://github.com/jamesfxley/pico-http/releases) include migration
steps.

## AI-assisted development

This project is developed with AI assistance. People direct design, review and
acceptance; agents write and refactor code against the engineering standard.
Changes pass the repository gates before merging.

The terms in [LICENSE](LICENSE) apply, including its disclaimer of warranties.
