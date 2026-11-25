# Using pico-http

This guide covers application setup, handlers, body storage and custom
transports. [Architecture](architecture.md) explains the implementation.

## Setting up

The firmware factory constructs the lwIP transport. The application
initialises CYW43 with `cyw43_arch_init()` and joins a network before
`start()`. The [quick start](../README.md#quick-start) covers build inputs.

<!-- example: setting_up -->
```cpp
#include <pico_http/pico_http.hpp>

auto server = pico_http::make_http_service();
const bool routed = server->add_route(
    "GET", "/status",
    [](const pico_http::HTTPRequestView&, pico_http::HTTPResponse& res) {
        res.set_body("ok");
        return pico_http::Disposition::COMPLETE;
    });
if (!routed) { /* the method or pattern is too long */ }
if (!server->start(80)) { /* port bind failed */ }
```

Eight fixed connection slots make `HttpService` too large for the main stack.
`make_http_service()` returns a `unique_ptr`. Construction, registration and
`start()` form the one-time initialisation budget; static and generator
requests allocate nothing afterwards (AR-1).

Transport callbacks drive the server after `start()`. The main loop only needs
to remain alive.

## Routes and handlers

<!-- example: routes -->
```cpp
if (!server->add_route("GET", "/exact", handler) ||     // exact match
    !server->add_route("GET", "/files/*", handler)) {   // trailing-* prefix match
    /* a method or pattern is too long */
}
server->set_default_handler(handler);             // "/" and empty URI
server->set_404_handler(handler);                 // everything else
```

A `GET` route also handles `HEAD`, with the body suppressed (NR-9). An
explicit `HEAD` route overrides that fallback. Registering a method and
pattern again replaces the handler. Exact matches take precedence over
wildcard matches.

A handler reads `HTTPRequestView` and fills `HTTPResponse`. It returns
`COMPLETE` to send now or `PENDING` to resolve later.

Request views last until handler return for `COMPLETE` or request resolution
for `PENDING`. The pooled `HTTPResponse&` lasts only for the call. A deferring
handler must not keep it.

Handlers run in the async context. Keep them short and never block. A handler
with lengthy work should defer.

## Deferring a response

A handler waiting for flash, a sensor or another bus can return and answer
later:

<!-- example: deferring -->
```cpp
// Fills the response at completion time. A plain function pointer plus ctx,
// A capturing lambda cannot convert to ResponseFiller.
void fill_reading(void* ctx, pico_http::HTTPResponse& res) {
    res.set_body(static_cast<Reading*>(ctx)->json(), "application/json");
}

void serve_readings(pico_http::HttpService& server, Reading& reading) {
    const bool routed = server.add_route(
        "GET", "/reading",
        [&reading](const pico_http::HTTPRequestView& req,
                   pico_http::HTTPResponse&) {
            reading.pending = req.request;  // keep only this
            reading.start();
            return pico_http::Disposition::PENDING;
        });
    if (!routed) { /* the method or pattern is too long */ }
}

// Later, from your main loop, another core or an ISR, once the reading is in:
void reading_ready(pico_http::HttpService& server, Reading& reading) {
    server.respond(reading.pending, &fill_reading, &reading);
}
```

Retain `req.request` for completion. Do not retain `HTTPResponse&`: its pool
entry is reused when the handler returns. The service invokes the filler
against a valid response only while the request is live. A `PENDING` handler
should write nothing to the response; its contents are discarded.

Deferred completion has these properties:

- A stale or duplicate `respond()` returns `false` without running the filler.
  Disconnect, timeout, `stop()` and a later request all make the old id inert.
- `respond()` records the completion and signals the transport. It makes no
  lwIP call. The transport builds and sends the response later, so publication
  is safe from another core or an ISR.
- While deferred, buffered peer bytes hold the receive window closed. Further
  requests wait until completion.
- `MAX_PENDING_RESPONSES` limits deferred requests to four by default. A
  further `PENDING` receives 503 with `Connection: close`; inline handlers on
  other connections remain available (BR-12).
- A deferred request must be answered within `handler_ms`, default 10 s, or
  teardown increments `disconnect_timeout_handler`. Raise the timeout for longer
  work (TR-6).

## The three body modes

| Mode | API | Copies | When |
|---|---|---|---|
| Buffered | `set_body(string_view / span)` | body → response; the slot takes that buffer over | Small dynamic bodies, up to `MAX_BUFFERED_BODY`; a larger one is answered `500` (BR-14) |
| Static | `set_body_static(StaticBody)` | zero | Fixed strings, flash-resident pages |
| Generator | `set_body_generator(fn, ctx, length)`, or without `length` for a chunked/close-delimited body | producer fills scratch on demand | Arbitrarily large responses in bounded RAM; length-less for unknown sizes |

Static body memory must outlive the response. Backpressure can delay reads
beyond handler return (AR-11). `StaticBody` has no implicit span or
string-view conversion:

<!-- example: static_body -->
```cpp
res.set_body_static(pico_http::StaticBody::outliving_response("not found"),
                    "text/plain");
res.set_body_static(pico_http::StaticBody::outliving_response(flash_page),
                    "text/html");
```

`outliving_response()` declares the lifetime. Literals, flash pages and static
buffers qualify; stack and request storage do not. Use buffered mode for
short-lived storage. The factory records the claim but cannot prove it.

A generator must be a pure function of byte offset, without an internal
cursor. The writer owns the position and may request a range again after a
partial send:

<!-- example: generator -->
```cpp
std::size_t pattern(void* /*context*/, std::size_t offset, std::span<std::uint8_t> out) {
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>('A' + ((offset + i) % 26U));
    }
    return out.size();
}
void serve_pattern(pico_http::HTTPResponse& res) {
    res.set_body_generator(pattern, nullptr, std::size_t{256} * 1024,
                           "application/octet-stream");
}
```

Returning zero before a known length is reached closes the connection with a
truncated response.

A generator without a declared length returns zero at end-of-body. HTTP/1.1
uses chunked framing; HTTP/1.0 uses socket close to delimit the raw body. The
pure-function contract also applies.

<!-- example: chunked_generator -->
```cpp
res.set_body_generator(pattern_until_done, &state, "application/octet-stream");
```

## Framing, keep-alive, timeouts

The service derives Content-Length or Transfer-Encoding from the body mode and
drops handler-set framing headers. An unchecked handler length could
desynchronise the next response on a persistent connection. Handler Connection
headers override version defaults (NR-2), but cannot override the request cap
or close-delimited framing. A forced close emits `Connection: close` and drops
handler persistence headers. Chunked responses retain keep-alive.

<!-- example: keep_alive -->
```cpp
server->set_keep_alive_config({.enabled = true,
                               .idle_timeout_seconds = 15,
                               .max_requests_per_connection = 16});
server->set_timeout_config({.header_ms = 5000,     // TR-1 slowloris window
                            .body_ms = 5000,       // TR-2 body progress
                            .write_ms = 10000,     // TR-4 write stall
                            .handler_ms = 10000}); // TR-6 deferred handler
```

Requests carrying `Expect: 100-continue` get the interim response
automatically once the head passes the limit checks (NR-5).

## Limits and errors

Boundary limits are compile-time policy (`DefaultHttpLimits`): 512 B URI,
322 B header line (64 B name + 256 B value + `: `, derived from the header
caps), 16 headers, 2,048 B head, 2,048 B buffered body. Requests
over a limit get the correct status (`414`/`431`/`413`), malformed requests
`400`, unsupported transfer codings `501`; always with `Connection: close`.
Chunked request bodies are decoded transparently; the handler sees the
assembled body either way.

Protocol conformance is enforced before dispatch (NR-1..NR-10): a request
with more than one `Host` header, or an HTTP/1.1 request without one, → `400`;
a well-formed but unsupported HTTP version (e.g. `HTTP/2.0`) → `505`; a control
character in a field value → `400`; an `Expect` other than `100-continue` →
`417`; `Transfer-Encoding` on an HTTP/1.0 request → `400`. Repeated header
names are combined into one comma-separated value before the handler sees
them. A request whose URI is registered only under other methods → `405` with
an `Allow` header listing them, rather than `404`.

## Observability

<!-- example: observability -->
```cpp
const auto& stats = server->statistics();   // OR-1 counter set
server->reset_statistics();
const std::size_t live = server->active_connections();
```

See `HttpService::Statistics` for the full counter list, and the
[benchmark firmware](../examples/bench/main.cpp) for serving it as JSON
(`/stats`, `/limits`, `/info`).

## Testing on the host

Inject `FakeTcpTransport` to exercise the service without hardware or sockets:

<!-- example: host_testing -->
```cpp
#include <pico_http/testing/fake_tcp_transport.hpp>

auto transport = std::make_unique<pico_http::testing::FakeTcpTransport>();
auto* wire = transport.get();
pico_http::HttpService server(std::move(transport));
server.start(80);

auto id = wire->connect();                          // accept a connection
wire->deliver_text(id, "GET / HTTP/1.1\r\n\r\n");   // bytes from the peer
wire->deliver_ack(id, wire->send_inflight(id));     // peer "reads" (frees credit)
const std::string response = wire->sent_text(id);         // everything sent so far
```

The fake models the real transport's semantics: credit-based sends against a
configurable capacity (`set_send_capacity`) for backpressure tests, receive
window accounting, RST injection (`reset_peer`), peer close (`close_peer`),
and virtual time (`advance_time`) for exercising every timeout kind. The
service-level test suite (`tests/http/http_service_test.cpp`) is a working
reference for all of these patterns.

## Porting: a custom transport or profile

### A custom transport

A custom `ITcpTransport` lets the HTTP layer use another network stack.

`TransportBase` provides generation-tagged slots (AR-4), deadlines (AR-7),
drain-close (TR-5) and deferred disconnect notification. Derive with a slot
type containing `close_pending` and implement four hooks:

<!-- example: porting_transport -->
```cpp
struct MySlot {
    bool close_pending = false;
    // ... whatever your stack needs per connection
};

class MyTransport
    : public pico_http::tcp::TransportBase<MySlot, pico_http::MAX_CONNECTIONS> {
 protected:
    [[nodiscard]] std::uint32_t clock_ms() const override;  // your clock
    [[nodiscard]] bool slot_drained(const MySlot& slot)
        const override;  // all bytes acked?
    void set_close_pending(MySlot& slot) override;
    void release_slot_resources(ConnectionId client, MySlot& slot,
                                pico_http::DisconnectReason reason) override;  // free, no callbacks

 public:
    bool listen(std::uint16_t port) override;
    std::size_t send(ConnectionId client,
                     std::span<const std::uint8_t> data) override;
    void consume(ConnectionId client, std::size_t bytes) override;
    void close_server() override;
};
```

The host tests check these transport contracts:

- `send()` never truncates silently (AR-5). Return how many bytes were
  accepted; the writer resumes the remainder on the next writable event.
  Report free ring space from `send_credit()` so a generator body can size its
  next fill to what will actually be accepted.
- Received bytes are acknowledged explicitly (AR-6). Deliver bytes with
  `notify_receive()`, but reopen the receive window only when the application
  calls `consume()`. Unconsumed bytes must hold the window closed; that is
  the flow control.
- Every entry point that can reach application code opens a
  `CallbackScope`. This is what defers disconnect notification until the
  outermost callback unwinds, so a handler closing its own connection cannot
  re-enter the service while it still holds that connection's state. It
  includes whatever drives `notify_poll()`: `HttpService::process()` holds a
  reference to the connection across that call, and its stability argument is
  the deferral.
- Deferred responses are a transport capability (AR-9). A handler returning
  `Disposition::PENDING` is answered later, from the transport's own context.
  Override `signal_completion()` to ask for an `on_poll()` on a later turn of
  that context; or drive `notify_poll()` yourself, as `FakeTcpTransport` does.
  A transport that does neither still gets `true` back from `respond()`, and
  the response is simply never written: the connection hangs until the TR-6
  timeout. Override `lock_completions()`/`unlock_completions()` too wherever
  `respond()` can be called from another core or an ISR, and give that lock the
  transport's whole lifetime; `HttpService::stop()` takes it after
  `close_server()`, and a service whose `listen()` failed takes it having never
  listened.

`is_open()` and the event-sink plumbing come from the base. `FakeTcpTransport`
is a complete worked example.

### A custom profile

A profile is a struct of compile-time sizes satisfying the `HttpProfile`
concept (`pico_http/config/profile.hpp`). Point
`PICO_HTTP_ACTIVE_PROFILE` at your own to retune every static allocation:

<!-- example: porting_profile -->
```cpp
struct MyProfile {
    using Limits = pico_http::DefaultHttpLimits;   // or your own HttpLimits
    static constexpr std::size_t MAX_CONNECTIONS = pico_http::MAX_CONNECTIONS;
    static constexpr std::size_t MAX_PENDING_RESPONSES =
        pico_http::MAX_PENDING_RESPONSES;
    static constexpr std::size_t HEAD_CAPACITY = 1024;
    static constexpr std::size_t GENERATOR_CHUNK = 512;
    static constexpr std::size_t RX_CAPACITY = pico_http::ADVERTISED_RX_WINDOW;
};
```

Profile sizes have these checked constraints:

- `RX_CAPACITY` must be at least `ADVERTISED_RX_WINDOW`, or a full window would
  not fit in the assembly buffer and delivered bytes would be dropped (AR-6).
  `HttpService` `static_assert`s this.
- `MAX_PENDING_RESPONSES` must be between 1 and `MAX_CONNECTIONS` (BR-12). Zero
  would refuse every deferral with `503`; a cap above the slot count is
  unreachable, because a connection can park at most one request.
- The buffer sizes must stay paired with `lwipopts.h`: the receive window with
  `TCP_WND`, the TX ring with `TCP_SND_BUF` and the MSS with `TCP_MSS` are
  `static_assert`ed, so changing one without the other fails the build instead
  of silently under-sizing. The specification's AR-10 lists every requirement.

Define profile macros for every translation unit including the headers. They
change object layout. The CMake profile option propagates them automatically;
a custom build must supply them everywhere. `HttpService` compares caller and
library sizes at construction and aborts on mismatch to prevent memory
corruption.
