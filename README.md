# pico-http

[![CI](https://github.com/jamesfxley/pico-http/actions/workflows/ci.yml/badge.svg)](https://github.com/jamesfxley/pico-http/actions/workflows/ci.yml)

`pico-http` is a modern C++20 library for building lightweight, robust HTTP
servers on the Raspberry Pi Pico 2 W. The core is **statically
allocated**: fixed connection slots, fixed parser/response buffers, and a
zero-copy transmit ring — no post-init heap growth on the hot paths, no silent
truncation under load, and hard boundary limits that answer with a real HTTP
status, except the connection cap, which is refused at the TCP layer.

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

lwIP requires the application to provide `lwipopts.h`, and
`PICO_LWIPOPTS_TARGET` **must** name the INTERFACE target carrying yours;
configuration fails without it. The SDK compiles lwIP into this library, so a
default would build a second lwIP against a configuration your application does
not use. `pico_http::lwipopts_fallback` is a reference configuration that
meets pico-http's requirements, to start yours from.

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

Responses can be supplied three ways — copied (`set_body`), a view over static
memory copied only into the transmit ring (`set_body_static`), or streamed from a resumable generator
(`set_body_generator`, arbitrarily large responses in bounded RAM). A generator
given a length produces a `Content-Length` response; the length-less overload
produces a `Transfer-Encoding: chunked` response (close-delimited for HTTP/1.0
clients) for bodies whose size is not known up front. `HEAD` is served
automatically wherever a `GET` route exists. See the
[usage guide](docs/usage-guide.md).

On the host, inject the fake transport instead of lwIP (this is what the test
suite does — no hardware required):

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
| `pico_http::testing` | `FakeTcpTransport` and `ServiceHarness` — drive the HTTP layer from host tests (deliver bytes, ack sends, inject RSTs, advance time). Exported with the package from a host build; a firmware build exports `tcp_lwip` instead |
| `pico_http::core` | Umbrella INTERFACE target linking the library targets above |

Product dependency pins for pico-logging and pico-util live in CMakeLists.txt.
The `infra` submodule supplies the pinned family and House build support.
Source overrides are explicit; sibling checkouts are not discovered.

## Building & tests

Initialize the pinned family and its nested House and standard with
`git submodule update --init --recursive`. The family supplies the build helpers,
gates and devcontainer; product revisions are declared in CMakeLists.txt.

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

The choice feeds the `HttpService` profile, the transport buffer factors, and
lwipopts together, and static_asserts bind each buffer to the lwipopts value it
depends on so the two cannot drift; the specification's AR-10 lists every
requirement on an application's `lwipopts.h`. The definitions are usage
requirements of `pico_http::config`, so a consumer's own translation units are
compiled against the same layout as the library it links. Supply a fully custom
profile by defining `PICO_HTTP_ACTIVE_PROFILE` to a struct satisfying the
`HttpProfile` concept (see `pico_http/config/profile.hpp`). Header caps are
independent knobs (`PICO_HTTP_MAX_HEADER_NAME` /
`PICO_HTTP_MAX_HEADER_VALUE`).

## On-device records

The on-device bench suite covers chunked request decode,
`Expect: 100-continue`, slowloris/timeout defenses, RST robustness and zero
post-init heap growth on a Pico 2 W (RP2350). No reference records are committed yet;
they will live in [examples/bench/baselines/](examples/bench/baselines/)
(FW-4). Some tests record numbers without a verdict;
[examples/bench/baselines/README.md](examples/bench/baselines/README.md) names
them, and "green" is a claim about the tests that adjudicate.

- **Supported target: Pico 2 W (RP2350).** The Pico W (RP2040) is refused at
  configure: pico-logging's atomics are not lock-free on its ARMv6-M core
  (FW-1).
- **Copied-body allocation is bounded, not zero** — a copied `set_body` holds
  its body in the connection slot until sent, and one over `MAX_BUFFERED_BODY`
  is answered `500` instead; static and generator bodies allocate zero. The
  bound is AR-1's, with BR-14.
- **Second-core handler execution and server-side TLS** are outside the supported surface (FW-2, FW-3).

## Documentation

- [Documentation index](docs/README.md) — what each document is for
- [Orientation](docs/orientation.md) — commands, conventions and traps, before
  a first change
- [Contributing](CONTRIBUTING.md)
  — how a change is proposed, and what the gates ask of it
- [Technical specification](docs/technical-specification.md) — normative;
  requirement IDs `AR/PR/RR/BR/TR/NR/OR`, their pass criteria and the
  traceability matrix
- [Architecture](docs/architecture.md) — how the stack works: connection
  slots, RX/TX paths, backpressure, timeouts, observability
- [Usage guide](docs/usage-guide.md) — routes, body modes,
  configuration, host testing
- [Benchmark suite](examples/bench/README.md) — on-device endpoint
  contract and `bench.py` results
- [Standing deviations](docs/deviations.md) — scope-wide rule suppressions and
  their review dates
- [Releases](https://github.com/jamesfxley/pico-http/releases) — what changed per
  release, and what to change in your code when an API broke

## Releases

Versioning is semver: a breaking change lands at a minor bump while the major
version is 0, and at a major bump from 1.0. The notes on
each [release](https://github.com/jamesfxley/pico-http/releases) record what
changed and, for a break, what to change in your own code (`EN-06.3`).

## AI-assisted development

This project is developed with AI assistance. Design decisions, review and
acceptance are human; an AI agent writes and refactors code under direction and
against a written engineering standard. Every change passes the repository's own
gates — build, tests, formatter, linter, deviation counts — before it lands.

This section is disclosure, not a warranty. The terms in [LICENSE](LICENSE)
apply unchanged, including its disclaimer of warranties.
