# Using pico-http

Practical guide to building on `pico_http::HttpService` — the static-core
HTTP/1.1 server. For how it works underneath, see
[architecture.md](architecture.md).

## Setting up

Firmware builds get a convenience factory that wires the lwIP transport. The
application initialises CYW43 (`cyw43_arch_init()`) and joins a network before
`start()`; the build side, including `PICO_LWIPOPTS_TARGET`, is the README's
Quick start.

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

`HttpService` holds eight statically-sized connection slots, which makes it too
large for `main()`'s stack — `make_http_service()` returns a `unique_ptr` for
this reason. Construction, route registration, and `start()` are the one-time init
budget (AR-1); after that the static and generator paths allocate nothing per
request.

Everything after `start()` is event-driven from the cyw43 async context; your
main loop only needs to stay alive (e.g. the benchmark's heartbeat loop).

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

`HEAD` requests are answered wherever a `GET` route exists (NR-9): the `GET`
handler runs and the response carries its header section with no body. Register
an explicit `HEAD` route to override that. Re-registering a method + pattern
replaces the earlier handler; exact matches win over wildcard matches.

A handler receives the parsed request as an `HTTPRequestView` (string views over
the method, URI, version, headers, and body) and fills in an `HTTPResponse`. It
returns `Disposition::COMPLETE` when the response is ready to send, or
`Disposition::PENDING` when it will answer later (below).

The views are valid for as long as the request is: until the handler returns for
a `COMPLETE` handler, and until the request resolves for a deferred one. The
`HTTPResponse&` is library-owned in both cases — it comes from a pool, never
your caller's stack — and is valid **only for the duration of the call**. A
deferring handler must not keep it; see below.

Handlers run in the async context: keep them short and never block. A handler
that has real work to do should defer rather than do it here.

## Deferring a response

A handler that would otherwise block — waiting on a flash write, a sensor
sample, another bus — can hand the request back and answer later:

<!-- example: deferring -->
```cpp
// Fills the response at completion time. A plain function pointer plus ctx,
// not a capturing lambda: capturing would allocate, and AR-1 forbids that.
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

`req.request`, kept here as `reading.pending`, is the only thing you keep. **Do not hold the `HTTPResponse&`** —
it belongs to the service and is reused by another connection as soon as your
handler returns, so anything you write to it after that lands in someone else's
response. That is why the filler exists: the service calls it against a response
it hands you at completion time, and only if the request is still live. A
handler that returns `PENDING` should write nothing to the response it was
given; it is discarded.

Five rules make this safe:

- **`respond()` is inert on a request that has gone away** — the connection
  timed out, was reset, was closed by `stop()`, or has already moved on to a
  later request. It returns `false` and your filler simply never runs; there is
  no race to avoid and nothing to check first. A second `respond()` for the same
  request is likewise inert.
- **`respond()` performs no send.** It records the completion and asks the
  transport for a turn; the response is built and sent on that turn. This is
  what makes it safe to call from another core or an ISR — it makes no lwIP
  call and holds the transport's completion lock only for the stores that
  publish the record.
- **The connection reads nothing more while deferred.** Bytes the peer sends
  stay buffered and hold the receive window closed, so a deferring handler
  applies backpressure rather than letting work pile up behind it.
- **Deferrals are capped** by `MAX_PENDING_RESPONSES` (default 4, BR-12). Past
  that, a handler that returns `PENDING` is refused with `503` and
  `Connection: close` — deferral is a bounded resource, not a queue. Handlers
  that answer inline are not subject to the cap, so slow deferred work cannot
  refuse ordinary traffic on other connections.
- **A deferred request must be answered within `handler_ms`** (TR-6, default
  10 s) or the connection is torn down and counted in
  `disconnect_timeout_handler`. Raise it if your work legitimately takes longer.

## The three body modes

| Mode | API | Copies | When |
|---|---|---|---|
| Buffered | `set_body(string_view / span)` | body → response; the slot takes that buffer over | Small dynamic bodies, up to `MAX_BUFFERED_BODY`; a larger one is answered `500` (BR-14) |
| Static | `set_body_static(StaticBody)` | zero | Fixed strings, flash-resident pages |
| Generator | `set_body_generator(fn, ctx, length)`, or without `length` for a chunked/close-delimited body | producer fills scratch on demand | Arbitrarily large responses in bounded RAM; length-less for unknown sizes |

**Static contract:** the memory must outlive the response — it is read again
on backpressure resume, potentially long after the handler returned. The type
carries the claim rather than leaving it to be remembered (AR-11), and there is
no conversion from a span or a string view, so there are two ways in:

<!-- example: static_body -->
```cpp
res.set_body_static(pico_http::StaticBody::outliving_response("not found"),
                    "text/plain");
res.set_body_static(pico_http::StaticBody::outliving_response(flash_page),
                    "text/html");
```

Naming `outliving_response()` is you vouching for the storage: string literals,
flash pages and `static` buffers qualify; a stack buffer does not, and neither
does anything reached through the request, whose storage the connection recycles
the moment your handler returns. Use buffered mode for both of those. The factory
does not make the claim true — it makes it impossible to make by accident, and
visible to a reader where it is made.

**Generator contract:** the producer must be a *pure function of the byte
offset* — no internal cursor. The writer owns the position and may re-request
any range (partial sends resume mid-body), so statelessness is what makes
resumption safe:

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

Returning 0 before `length` is reached is treated as a broken generator: the
response is cut short and the connection closed.

**Length-less (chunked) generator:** the overload without a `length` argument
streams a body whose total size is not known up front. The generator returns 0
to signal end-of-body (not an error, unlike the lengthed form). To an HTTP/1.1
client the response is framed as `Transfer-Encoding: chunked`; an HTTP/1.0
client — which cannot parse chunked framing — gets the raw body with
`Connection: close`, the socket close delimiting it. The same pure-function
contract applies.

<!-- example: chunked_generator -->
```cpp
res.set_body_generator(pattern_until_done, &state, "application/octet-stream");
```

## Framing, keep-alive, timeouts

The service adds `Content-Length` (or `Transfer-Encoding: chunked` for a
length-less generator body), `Connection`, and (when applicable) `Keep-Alive`
headers automatically. Framing is the service's alone: it is derived from the
body mode the handler set, and a `Content-Length` or `Transfer-Encoding` set on
the response is dropped rather than echoed. Nothing compares a handler's value
against the body it then sends, so honouring one would let a wrong length reach
the wire and desync the next response on a keep-alive connection. An explicit
response `Connection` header overrides the version default — HTTP/1.1 defaults
to keep-alive, HTTP/1.0 to close (NR-2) — but not the two things that are not
preferences: `max_requests_per_connection`, which bounds how long one peer holds
a slot, and a framing decision that forces the connection closed (the HTTP/1.0
close-delimited downgrade below). Where the service closes, it emits
`Connection: close` and suppresses a handler-set `Connection`/`Keep-Alive`
rather than echoing it, so the head never announces persistence the server will
not honour. Chunked responses are self-delimiting, so keep-alive is preserved
across them.

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
`400`, unsupported transfer codings `501` — always with `Connection: close`.
Chunked request bodies are decoded transparently; the handler sees the
assembled body either way.

Protocol conformance is enforced before dispatch (NR-1..NR-10): an HTTP/1.1
request must carry exactly one `Host` header (missing or duplicated → `400`);
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

Inject `FakeTcpTransport` and drive the wire directly — no hardware, no
sockets:

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

`HttpService` talks to the network only through `ITcpTransport`. Implementing
it against another stack is the supported way to run this server somewhere
other than lwIP.

Derive from `pico_http::tcp::TransportBase`, which already provides the parts
every transport needs identically — the generation-tagged slot table (AR-4),
the timeout scheduler (AR-7), the drain-close lifecycle (TR-5), and deferred
disconnect delivery. Supply a slot payload with a `close_pending` member and
four hooks:

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

Four contracts are load-bearing, and the host tests assert all of them:

- **`send()` never truncates silently** (AR-5). Return how many bytes were
  accepted; the writer resumes the remainder on the next writable event.
  Report free ring space from `send_credit()` so a generator body can size its
  next fill to what will actually be accepted.
- **Received bytes are acknowledged explicitly** (AR-6). Deliver bytes with
  `notify_receive()`, but reopen the receive window only when the application
  calls `consume()`. Unconsumed bytes must hold the window closed — that is
  the flow control.
- **Every entry point that can reach application code opens a
  `CallbackScope`.** This is what defers disconnect notification until the
  outermost callback unwinds, so a handler closing its own connection cannot
  re-enter the service while it still holds that connection's state. It
  includes whatever drives `notify_poll()`: `HttpService::process()` holds a
  reference to the connection across that call, and its stability argument is
  the deferral.
- **Deferred responses are a transport capability** (AR-9). A handler returning
  `Disposition::PENDING` is answered later, from the transport's own context.
  Override `signal_completion()` to ask for an `on_poll()` on a later turn of
  that context — or drive `notify_poll()` yourself, as `FakeTcpTransport` does.
  A transport that does neither still gets `true` back from `respond()`, and
  the response is simply never written: the connection hangs until the TR-6
  timeout. Override `lock_completions()`/`unlock_completions()` too wherever
  `respond()` can be called from another core or an ISR, and give that lock the
  transport's whole lifetime — `HttpService::stop()` takes it after
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

Three rules the build enforces rather than trusts:

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

**Define the profile macros for every translation unit that includes these
headers, not just for the library.** They change class layout, so a consumer
compiled with different values than the library disagrees with it about the
size of `HttpService`, `HTTPHeaders` and the connection slot. Setting
`-DPICO_HTTP_PROFILE=...` on this project propagates them as usage
requirements automatically; a hand-rolled build must pass them everywhere.
`HttpService`'s constructor compares the caller's sizes against the library's
and aborts on a mismatch, because the alternative is memory corruption with no
other symptom.
