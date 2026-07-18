# prism

[![CI](https://github.com/jorgen/prism/actions/workflows/ci.yml/badge.svg)](https://github.com/jorgen/prism/actions/workflows/ci.yml)

A lean REST service library for **C++23**, built on
[vio](https://github.com/jorgen/vio) (async I/O: libuv + coroutines + TLS) and
[structify](https://github.com/jorgen/structify) (header-only JSON / YAML / CBOR ↔ struct).

The name continues vio's light theme: libuv is *ultraviolet*, vio is *violet io*,
and a **prism** disperses one request stream into a spectrum of routes.

```cpp
#include <prism/prism.h>
#include <vio/run.h>

struct greeting_t { std::string message; STFY_OBJ(message); };

vio::task_t<prism::negotiated_t<greeting_t>> hello(prism::path_t<"name", std::string> name)
{
  co_return prism::ok(greeting_t{"hello " + name.value});
}

void routes(prism::app_t &app)
{
  app.get("/hello/{name}", hello);
}

VIO_MAIN(loop, argc, argv)
{
  co_return co_await prism::run(loop, "127.0.0.1", 8080, routes);
}
```

Return a struct and prism serialises it in whatever the client asked for:

```console
$ curl localhost:8080/hello/ada
{"message":"hello ada"}
$ curl -H 'Accept: application/yaml' localhost:8080/hello/ada
message: hello ada
```

## Features

- **Coroutine handlers** — every handler returns `vio::task_t<response_t>`, so it
  can `co_await` any async vio operation (timers, outbound I/O, a database call)
  without blocking the event loop.
- **Multi-threaded by default** — prism runs one event loop per core (SO_REUSEPORT),
  scaling across cores out of the box; drop to a single loop with one option.
- **HTTP/1.1 server** — keep-alive, request pipelining, and slow-loris hardening
  (idle / header / body / write timeouts, `max_requests`, `max_connections`),
  parsing with [llhttp](https://github.com/nodejs/llhttp).
- **HTTP/2** — a hand-rolled stack (binary framing + HPACK + flow control + stream
  multiplexing, no extra dependency): cleartext **h2c** and **h2 over TLS with
  ALPN**, passing the full [h2spec](https://github.com/summerwind/h2spec)
  conformance suite. Your handlers don't change — HTTP/2 is just another transport
  under the same request/response/routing/negotiation layers.
- **WebSockets** — `app.ws(pattern, handler)` runs a long-lived coroutine per
  connection, and one handler serves `ws://`, `wss://`, and WebSocket over HTTP/2
  (RFC 8441); outbound sends are queued through the connection's writer, so
  pushing from a pub/sub broadcast or a timer is safe.
- **Typed route parameters** — bind `path_t<"id", int>`,
  `query_t<"q", std::optional<int>>`, and `body_t<T>` (parsed by `Content-Type`)
  straight into the handler signature; prism parses, validates, and answers `400`
  *before* your code runs.
- **JSON / YAML / CBOR — negotiated** — request/response bodies are plain structs
  with a single `STFY_OBJ(...)` line; prism picks the response format from the
  `Accept` header (`406` if it can't) and parses the request body by its
  `Content-Type` (`415` / `400`).
- **Streaming both ways** — produce a response body incrementally, or consume a
  huge upload without buffering it; chunked on HTTP/1.1, flow-controlled DATA
  frames on HTTP/2, with real backpressure either way.
- **Static files** — serve a directory of built assets (an SPA, a bundle) next to
  your API with one call.
- **Pluggable logging** — a `std::function` sink with a level filter; stdout by
  default, swap in your framework in one line.
- **Lean & consistent** — errors flow through `result_t<T>`
  (`std::expected<T, error_t>`), mirroring vio so the two libraries feel like one;
  prism's own code is compiled `-fno-exceptions -fno-rtti`.

## Routing

Patterns are matched segment by segment. A `{name}` segment captures a path
parameter; a `?key=value` query string is read separately and never affects
routing. Dispatch yields `404` when no path matches and `405` when the path
matches but the method does not.

```cpp
app.get("/health", health);                 // static
app.get("/tasks/{id}", get_task, store);    // {id} captured
app.post("/tasks", create_task, store);     // method-specific
```

### Typed parameters (recommended)

Declare what you need in the signature and let prism extract it. Bound state
(`store`) comes first; extractors follow.

```cpp
struct task_t { int id; std::string title; bool done; STFY_OBJ(id, title, done); };

// {id} parsed to int; an invalid id is rejected with 400 before this runs
vio::task_t<prism::response_t> get_task(std::shared_ptr<store_t> store,
                                        prism::path_t<"id", int> id)
{
  if (task_t *t = store->find(id.value)) co_return prism::json::respond(prism::status_t::ok, *t);
  co_return prism::response_t::text(prism::status_t::not_found, "task not found");
}

// JSON request body bound to a struct (malformed body -> 400 automatically)
vio::task_t<prism::response_t> create_task(std::shared_ptr<store_t> store,
                                           prism::body_t<task_create_t> in) { /* in.value */ }

// optional query parameter: absent -> std::nullopt, ?done=true -> bool
vio::task_t<prism::response_t> list_tasks(std::shared_ptr<store_t> store,
                                          prism::query_t<"done", std::optional<bool>> done) { /* done.value */ }

app.get("/tasks/{id}", get_task, store);
app.post("/tasks",     create_task, store);
app.get("/tasks",      list_tasks, store);
```

Parsing goes through the `param_codec_t<T>` customization point (integral, bool,
and `std::string` built in — specialize it for your own types).

Routes are **verified** against the handler's `path_t<>` extractors. The runtime
form checks at startup — a handler binding `path_t<"id">` against a
`"/tasks/{ide}"` typo is reported by `app.route_errors()` and makes `listen()`
fail fast before binding. Or opt into **compile-time** verification by passing the
pattern as a template argument — the same typo is then a build error:

```cpp
app.get("/tasks/{id}", get_task, store);    // runtime: verified at startup; can register after listen()
app.get<"/tasks/{id}">(get_task, store);    // static:  verified at compile time
```

### The raw form

You can always take the `request_t` and read things by hand:

```cpp
vio::task_t<prism::response_t> handler(prism::request_t request)
{
  std::string_view id = request.param("id");    // path parameter
  std::string q       = request.query("q");      // query parameter (percent-decoded, + -> space)
  bool verbose        = request.has_query("v");
  co_return prism::response_t::text(prism::status_t::ok, "ok");
}
```

### Async handlers

Handlers are coroutines. Take a `request_t` to reach the event loop and
`co_await` anything vio offers:

```cpp
vio::task_t<prism::response_t> slow(prism::path_t<"ms", int> ms, prism::request_t request)
{
  co_await vio::sleep(*request.loop, std::chrono::milliseconds{ms.value});
  co_return prism::response_t::text(prism::status_t::ok, "done");
}
```

Prefer **free-function coroutine handlers** with state bound at registration over
capturing coroutine lambdas: a lambda's captures live in the closure, a vio
dangling-`this` footgun, whereas a free function takes its state into the
coroutine frame by value.

## Bodies

### Content negotiation

Wrap your value in `negotiated_t<T>` and prism chooses the wire format from the
request's `Accept` header — JSON, YAML, or CBOR — serialising through structify.
`prism::ok` / `prism::created` are shorthands; `prism::respond(status, value)`
takes an explicit status.

```cpp
vio::task_t<prism::negotiated_t<task_t>> get_task(std::shared_ptr<store_t> store,
                                                  prism::path_t<"id", int> id)
{
  if (task_t *t = store->find(id.value)) co_return prism::ok(*t);
  co_return prism::response_t::text(prism::status_t::not_found, "task not found");
}
```

- **Response**: `Accept: application/yaml` → YAML, `application/cbor` → CBOR,
  absent / `*/*` → JSON, anything unsatisfiable → `406`.
- **Request body**: `body_t<T>` parses per `Content-Type`
  (`application/json` / `application/yaml` / `application/cbor`); an unknown type
  is `415`, malformed bytes are `400`.
- A `response_t` returned from a negotiated handler (e.g. a differently-typed
  error body) is sent **as-is**, bypassing negotiation — so success and error
  bodies of different shapes coexist in one handler.

A classic `request_t` handler has no value wrapper, so negotiate explicitly:

```cpp
co_return prism::respond(request, prism::status_t::ok, value);
```

### Finished & binary bodies

Already hold the bytes — an image, a proxied payload, a hand-built CSV? Send them
verbatim with the content type you choose (the body is a binary-safe buffer, so
this covers CBOR, JPEG, anything):

```cpp
co_return prism::response_t::finished(prism::status_t::ok, "text/csv", std::move(csv));
```

Read a raw request body (uploads) as bytes with `request.raw_body()`
(`std::span<const std::byte>`); `body_t<T>` stays for structured input.
`prism::json::respond` is unchanged — the explicit, always-JSON path.

### Streaming responses

For a body you don't want to hold in memory — a large download, a generated
export, a server-sent event feed — return `response_t::streaming`. The source is
pulled for successive chunks until one reports `last`; it can `co_await` async work
(a file read, an upstream fetch) between chunks.

```cpp
vio::task_t<prism::response_t> download(prism::request_t)
{
  auto n = std::make_shared<int>(0);
  co_return prism::response_t::streaming(prism::status_t::ok, "text/plain",
    [n]() -> vio::task_t<prism::body_chunk_t>
    {
      if (*n >= 100) co_return prism::body_chunk_t{"", true};
      co_return prism::body_chunk_t{"line " + std::to_string((*n)++) + "\n", false};
    });
}
```

prism frames this as chunked transfer-encoding on HTTP/1.1 and as flow-controlled
DATA frames on HTTP/2 — with real backpressure either way, so a slow client can't
force the server to buffer the whole body.

### Streaming request bodies

Symmetrically, a very large **upload** can be consumed incrementally instead of
buffered whole. Register the route with `post_stream` (also `get_/put_/patch_/
del_stream`): the handler runs as soon as the headers are parsed and pulls the
body through `request_t::body_stream()`.

```cpp
app.post_stream("/upload",
  [](prism::request_t request) -> vio::task_t<prism::response_t>
  {
    std::array<std::byte, 64 * 1024> buffer{};
    std::uint64_t total = 0;
    prism::request_body_t &body = request.body_stream();
    for (;;)
    {
      std::size_t n = co_await body.read_into(std::span<std::byte>(buffer.data(), buffer.size()));
      if (n == 0) break;         // 0 == end of body
      total += n;                // write_to_disk(buffer.data(), n), hash, forward, ...
    }
    co_return prism::response_t::text(prism::status_t::ok, std::to_string(total) + " bytes\n");
  });
```

`read_into` fills your buffer and returns the byte count — for Content-Length
bodies it reads straight into your buffer (**zero-copy on plain TCP**; on TLS it
decrypts directly into it, the fewest copies TLS allows). Alternatively `co_await
request.body_stream().read_chunk()` yields owned chunks, or `read_all()` drains
the rest into a `std::string`. It works on HTTP/1.1 and HTTP/2 with real
backpressure (HTTP/2 replenishes its receive window only as you consume). Streaming
routes take a raw `request_t`; typed `body_t<T>` still expects a buffered body. See
[`examples/upload_stream.cpp`](examples/upload_stream.cpp).

## Static files

Serving a single-page app or a bundle of assets next to your API is one call.
`app.static_files(url_prefix, root)` mounts a directory: a `GET` under
`url_prefix` maps to a file under `root`, read asynchronously off the event loop.

```cpp
prism::app_t app;

app.get("/api/hello/{name}", api_hello);   // register REST routes first
app.static_files("/", "webroot");          // everything else -> ./webroot
```

- **Order matters** — `static_files` registers a wildcard route (internally
  `"<prefix>/{path...}"`), so register your REST routes *before* it; the first
  match wins, and a `"/"` mount would otherwise catch everything.
- A directory (or the bare prefix) serves `index.html`; a missing file is `404`.
- **Content-Type** comes from the extension (html, css, js, json, svg, png, wasm,
  …; `application/octet-stream` otherwise).
- **Path traversal is rejected** — `..` segments (raw or percent-encoded) never
  escape `root`.
- **Single-page apps** — pass `spa_fallback = true` and prism serves `index.html`
  for unmatched *navigation* paths (e.g. `/dashboard`), so a React/Vue/… router
  survives a full page reload; a missing *asset* (`/assets/x.js`) still 404s.
- Files are read whole (fine for typical web assets); response-body streaming for
  very large files is future work.

```cpp
app.static_files("/", "dist", /*spa_fallback=*/true);   // serve a built SPA
```

See [`examples/webapp.cpp`](examples/webapp.cpp) for a REST API plus a static
site, and [`examples/react-app/`](examples/react-app) for a full TypeScript React
+ Vite app — developed with hot reload against a live prism API, then deployed as
a single prism binary serving both the built SPA and the API.

## HTTP/2

prism speaks HTTP/2 with a **hand-rolled** stack — binary framing, HPACK header
compression, per-stream and connection flow control, and stream multiplexing —
with no HTTP/2 dependency. Everything above the wire is unchanged: the same
`request_t` / `response_t`, routing, typed parameters, and content negotiation
serve HTTP/1.1 and HTTP/2 alike. It passes the full h2spec conformance suite in
both modes.

**Cleartext (h2c)** — prior-knowledge, for internal services, proxies,
gRPC-style backends, and tools like `curl --http2-prior-knowledge`:

```cpp
prism::server_options_t options;
options.protocol = prism::protocol_t::h2c;
co_return co_await prism::run(loop, "127.0.0.1", 8080, routes, options);
```

**Over TLS with ALPN** — how browsers speak HTTP/2. `listen_tls` advertises
`h2, http/1.1` and routes each connection to the h2 or HTTP/1.1 driver by the
negotiated protocol:

```cpp
vio::ssl_config_t tls;
tls.cert_file = "cert.pem";
tls.key_file  = "key.pem";
// tls.alpn_protocols defaults to {"h2", "http/1.1"}

prism::app_t app;
routes(app);
co_return (co_await app.listen_tls(loop, "0.0.0.0", 8443, tls)).has_value() ? 0 : 1;
```

### Things to know when serving HTTP/2

- **Browsers require TLS + ALPN.** No browser does cleartext h2c, so a public,
  browser-facing HTTP/2 endpoint needs a certificate. h2c is for controlled
  environments (service-to-service, a reverse proxy in front, local tooling). h2c
  is prior-knowledge only — the `Upgrade: h2c` dance (removed in RFC 9113) is not
  supported.
- **Handlers run concurrently.** A single HTTP/2 connection multiplexes many
  streams, and prism dispatches each as its own coroutine, so several of your
  handlers can be in flight on one connection at once. Keep per-request state in
  the request (or share immutable state via `shared_ptr`) — the same guidance as
  the free-function-with-bound-state pattern used throughout.
- **Flow control and hardening are built in.** prism advertises
  `SETTINGS_MAX_CONCURRENT_STREAMS` and a header-list-size cap, honours the peer's
  send window, and enforces DoS caps (rapid-reset / CVE-2023-44487, SETTINGS
  flood, HPACK-bomb header-list bound, per-stream body cap) — a violation ends the
  stream (`RST_STREAM`) or the connection (`GOAWAY`). `write_timeout` also bounds a
  slow-reading TLS client (a timed-out write closes the connection).
- **Streaming is transport-aware.** `response_t::streaming` becomes flow-controlled
  DATA frames on HTTP/2, so a large streamed body respects the client's window.
- **Timeouts are coarser than HTTP/1.1.** The read loop uses a connection-level
  idle/header timeout rather than per-stream deadlines (future work).
- **WebSockets ride HTTP/2 too.** With any `app.ws` route registered, prism
  advertises `SETTINGS_ENABLE_CONNECT_PROTOCOL`, so a browser opens the WebSocket
  on the same h2 connection via RFC 8441 Extended CONNECT instead of falling back
  to HTTP/1.1 — one handler covers both. See [WebSocket](#websocket).

## WebSocket

`app.ws(pattern, handler)` registers a WebSocket route. The handler is a
long-lived **free-function coroutine** that takes the connection by
`std::shared_ptr<ws_connection_t>` (the vio dangling-`this` rule — never a
capturing coroutine lambda) and drives it: `co_await receive()` pulls inbound
`ws_message_t`s until `.ok` is `false` (the peer closed), `co_await
send_text(string_view)` / `send_binary(span<const std::byte>)` push outbound
ones, `close(code = 1000, reason = "")` ends it, and `request()` exposes the
upgraded `request_t` (path params via `request().param("name")`, headers, loop,
per-thread factories).

```cpp
vio::task_t<void> echo(std::shared_ptr<prism::ws_connection_t> conn)
{
  co_await conn->send_text("welcome");
  for (;;)
  {
    prism::ws_message_t msg = co_await conn->receive();
    if (!msg.ok) break;                       // peer closed — no more messages
    co_await conn->send_text("echo: " + msg.data);
  }
}

app.ws("/ws", echo);                          // one handler, every transport
```

- **One handler, every transport** — the same coroutine serves `ws://` over
  HTTP/1.1, `wss://` over TLS, and WebSocket over HTTP/2 via RFC 8441 Extended
  CONNECT. Whenever a `ws` route exists prism advertises
  `SETTINGS_ENABLE_CONNECT_PROTOCOL` on h2, so a browser that keeps the connection
  on h2 opens the WebSocket on *that same connection* instead of failing the
  upgrade or forcing an HTTP/1.1 fallback. Handler code is identical regardless of
  transport.
- **Push from any coroutine** — outbound frames are queued through the
  connection's own single-flight writer, so calling `send_text` / `send_binary`
  from a *different* coroutine — a pub/sub broadcast, a timer — is safe, not only
  from inside the handler. This is the enabler for fan-out and server push.
- **Protocol plumbing is internal** — ping/pong and the close handshake are
  handled for you, and fragmented messages are reassembled before delivery.
- **Keepalive reaps dead peers** — after `server_options_t::ws_ping_interval` of
  inbound silence (default 30 s; `0` disables) prism sends a Ping and, if a second
  interval passes with no Pong or frame, closes the connection — so a wedged or
  vanished peer is reaped instead of pinning it. Works over HTTP/1.1 and HTTP/2.
- Message size is capped by `max_body_bytes`; permessage-deflate and subprotocol
  negotiation are out of scope.

See [`examples/hello_websocket.cpp`](examples/hello_websocket.cpp) — a `/ws` echo
endpoint plus a tiny browser page (or try `websocat ws://127.0.0.1:8080/ws`).

## Reverse proxy

`prism::reverse_proxy_t` does host-based reverse proxying: `add_route(host,
backend_t{host, port})` maps an incoming `Host` — the `Host` header on HTTP/1.1,
or the `:authority` pseudo-header on HTTP/2 — to a backend, and `install(app)`
registers **both** the HTTP passthrough (any method, any path) and the WebSocket
passthrough in one call.

```cpp
prism::app_t app;
prism::reverse_proxy_t proxy;

proxy.add_route("app.example.com", prism::backend_t{"127.0.0.1", 9001});
proxy.add_route("api.example.com", prism::backend_t{"127.0.0.1", 9002});
proxy.install(app);                           // HTTP + WebSocket, in one call

co_return (co_await app.listen(loop, "", 8080)).has_value() ? 0 : 1;
```

- **Host → backend** — `backend_t` is `{ std::string host; std::uint16_t port; }`;
  an incoming host that matches a route is forwarded there, HTTP requests and
  WebSocket upgrades alike.
- **Wire it by hand if you prefer** — `proxy.handler()` returns a `handler_t` and
  `proxy.ws_handler()` a `ws_handler_t`, so `app.any(pattern, proxy.handler())`
  and `app.ws(pattern, proxy.ws_handler())` do exactly what `install` does.
- **Which hop speaks what** — the browser-to-gateway hop runs over HTTP/1.1 *or*
  HTTP/2 (WebSockets via RFC 8441); the gateway-to-backend hop is HTTP/1.1.
- **Terminate TLS at the edge** — pair with `app.listen_tls` so the proxy
  terminates TLS and speaks HTTP/2 to browsers while reaching backends over
  cleartext HTTP/1.1.

See [`examples/reverse_proxy.cpp`](examples/reverse_proxy.cpp) — host-based
routing configured from the command line, with HTTP and WebSocket passthrough
wired via `install()`.

## Running the server

`prism::run` builds the app, runs your `configure(app_t&)` callback, and listens;
pair it with `VIO_MAIN(loop, argc, argv) { ... }` (from vio), which supplies
`main` and the event loop. For finer control, drive `app_t` yourself with
`app.listen(...)` / `app.listen_tls(...)`.

### Server options

Tuning and hardening knobs travel in `server_options_t`, passed to `listen` /
`listen_tls` / `prism::run`. A value of `0` disables the relevant timeout or cap.

```cpp
prism::server_options_t options;
options.idle_timeout     = std::chrono::seconds{30};
options.header_timeout   = std::chrono::seconds{10};
options.body_timeout     = std::chrono::seconds{30};
options.write_timeout    = std::chrono::seconds{30};
options.max_requests     = 1000;   // per connection (0 = unlimited)
options.max_connections  = 1024;   // concurrent, per worker (0 = unlimited)
options.worker_threads   = 0;      // event-loop threads (0 = hardware_concurrency)
options.shutdown_timeout = std::chrono::seconds{10};  // per-loop graceful drain

co_return co_await prism::run(loop, "0.0.0.0", 8080, routes, options);
```

Size caps (`max_header_bytes`, `max_body_bytes`, `max_streaming_body_bytes`) round
out the hardening set — see [`server_options.h`](src/prism/server_options.h).

### Bind address & dual-stack

The `host` argument selects the bind address by family: an IPv6 literal (`"::1"`,
`"::"`) binds IPv6, anything else IPv4. An **empty string is the default and binds
`::` as dual-stack**, so `127.0.0.1`, `::1`, and `localhost` all reach the server
from one socket (falling back to `0.0.0.0` when IPv6 is unavailable). Prefer it
for local dev: tools like Node/Vite resolve `localhost` to `::1`, which an
IPv4-only listener would refuse.

### Workers & concurrency (SO_REUSEPORT)

prism is **multi-threaded by default**. `worker_threads` runs that many event
loops on that many threads, each binding the same port with `SO_REUSEPORT` so the
kernel load-balances connections across them; the caller's loop is worker 0 and
prism spawns the rest.

- `0` (**default**) → `hardware_concurrency()` — one loop per core.
- `1` → the single-loop model (no extra threads).
- `N` → exactly `N` loops.

```cpp
prism::server_options_t options;
options.worker_threads = 4;      // or leave 0 for one loop per core
co_await app.listen(loop, "", 8080, &cancel, options);
```

Two consequences in multi-worker mode: `max_connections` is **per worker** (the
process ceiling is `worker_threads * max_connections`), and a custom logger sink
must be **thread-safe** (the default stdout sink already is).

### Per-thread state

For state that shouldn't be shared between threads — a DB connection, a reusable
scratch buffer, an RNG — register a factory once on the app and take a
`prism::per_thread<T>` parameter. Each thread (worker) gets **one** instance
(created lazily) as a **mutable reference**; every handler that takes
`per_thread<T>` shares that one per-thread instance (the factory is keyed by type
on the app, so five handlers don't make five connections).

```cpp
app.provide_per_thread<Db>([] { return Db::connect(); });   // once, before listen()

// mutable Db& via *, ->, or .get(); shared const config bound by reference
vio::task_t<prism::response_t> get_user(const Config &cfg, prism::per_thread<Db> db,
                                        prism::path_t<"id", int> id)
{
  auto row = db->query(cfg.table, id.value);   // db is this thread's Db, mutable
  co_return prism::json::respond(prism::status_t::ok, row);
}

app.get("/users/{id}", get_user, std::cref(config));   // bound const& + per-thread Db
```

It's keyed off the connection's event loop, which is the per-thread identity (no
`thread_local`), so with the default multi-worker runtime you get one instance per
core with no code change. See
[`examples/per_thread_state.cpp`](examples/per_thread_state.cpp).

### Graceful shutdown

Pass a `vio::cancellation_t*` to `listen` and fire it to shut down. Every worker
stops accepting, in-flight requests are drained (bounded by `shutdown_timeout`),
and all worker threads are joined before `listen`'s task returns — no connection is
cut mid-response and no thread is force-stopped mid-drain.

### Logging

Each `app_t` owns a logger that defaults to a stdout sink (warnings/errors to
stderr) and logs one access line per request plus lifecycle events. Plug in your
own framework with a one-line sink (make it thread-safe if you run more than one
worker):

```cpp
app.logger().set_level(prism::log_level_t::debug);
app.logger().set_sink([](prism::log_level_t level, std::string_view message) {
  my_framework::log(static_cast<int>(level), message);
});
```

## Building

```bash
cmake --preset debug
cmake --build cmake-build-debug
ctest --preset debug                 # or: ./cmake-build-debug/tests/prism_tests
./cmake-build-debug/examples/task_api 8080
```

Dependencies — **vio**, **structify**, **llhttp**, and **doctest** — are fetched
and pinned (URL + SHA256) at configure time via
[cmake-dep](https://github.com/jorgen/cmake-dep); no manual setup.

### Sanitizers

`asan`, `tsan`, `ubsan`, and `msan` presets are available:

```bash
cmake --preset asan && cmake --build cmake-build-asan && ctest --preset asan
```

CI runs the suite under AddressSanitizer, ThreadSanitizer, and
UndefinedBehaviorSanitizer on Linux for every push and pull request.

### Dependencies & overrides

Each dependency can instead be consumed **pre-built** by flipping a per-dependency
option and pointing CMake at the install prefix (`…Config.cmake` on
`CMAKE_PREFIX_PATH`):

| Option (default `OFF`)        | Effect when `ON`                                   |
|-------------------------------|----------------------------------------------------|
| `PRISM_USE_SYSTEM_VIO`        | `find_package(vio)` instead of the bundled build   |
| `PRISM_USE_SYSTEM_STRUCTIFY`  | `find_package(structify)`                          |
| `PRISM_USE_SYSTEM_DOCTEST`    | `find_package(doctest)` (test build only)          |
| `PRISM_USE_SYSTEM_LLHTTP`     | `find_package(llhttp)`                             |

```bash
cmake --preset debug \
  -DPRISM_USE_SYSTEM_DOCTEST=ON -DPRISM_USE_SYSTEM_LLHTTP=ON \
  -DCMAKE_PREFIX_PATH="/path/to/prefix"
```

When an option is `ON`, prism does not fetch that dependency's sources at all — it
must be resolvable via `find_package`. These knobs are auto-declared by cmake-dep
from each `CmDepFetchPackage` call (prefix derived from the project name). The same
mechanism gives every dependency a `PRISM_<DEP>_VERSION` / `PRISM_<DEP>_URL` /
`PRISM_<DEP>_SHA256` cache variable, so you can fetch a different version/URL/hash
without editing the packages file — e.g. `-DPRISM_LLHTTP_URL=… -DPRISM_LLHTTP_SHA256=…`.

#### Overriding vio's dependencies through prism

vio carries the same per-dependency toggles (`VIO_USE_SYSTEM_LIBUV`,
`VIO_USE_SYSTEM_LIBRESSL`, `VIO_USE_SYSTEM_ADA`, `VIO_USE_SYSTEM_DOCTEST`,
`VIO_USE_SYSTEM_CMAKERC`). Because prism builds bundled vio via `CmDepAddPackage`
— which force-sets only `VIO_BUILD_*`/`VIO_INSTALL`, never the dependency toggles —
**these pass straight through from the top-level configure line**. So a prism
consumer can override any of vio's dependencies without editing vio.

Use a specific, pre-installed **LibreSSL** (e.g. a newer version):

```bash
cmake --preset debug \
  -DVIO_USE_SYSTEM_LIBRESSL=ON \
  -DLIBRESSL_ROOT_DIR="/opt/libressl-4.1.0"      # CMake's FindLibreSSL
```

Or have vio *fetch and build* a different version — every vio dependency exposes
`VIO_<DEP>_VERSION` / `VIO_<DEP>_URL` / `VIO_<DEP>_SHA256` cache variables:

```bash
cmake --preset debug \
  -DVIO_LIBRESSL_VERSION=4.0.0 \
  -DVIO_LIBRESSL_URL=https://ftp.openbsd.org/pub/OpenBSD/LibreSSL/libressl-4.0.0.tar.gz \
  -DVIO_LIBRESSL_SHA256=<sha256>
```

A fully pre-built stack — vio installed against system deps, then consumed by prism:

```bash
cmake -S vio -B vio-build -DVIO_INSTALL=ON \
  -DVIO_USE_SYSTEM_LIBUV=ON -DVIO_USE_SYSTEM_LIBRESSL=ON -DVIO_USE_SYSTEM_ADA=ON \
  -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build vio-build --target install

cmake --preset debug -DPRISM_USE_SYSTEM_VIO=ON -DCMAKE_PREFIX_PATH="$PREFIX"
```

`VIO_INSTALL` emits a relocatable `find_package(vio)` config only when vio is built
against system deps (a bundled, in-tree build vendors targets that aren't
installed); it installs the library and headers otherwise.

## Examples

- [`examples/hello_prism.cpp`](examples/hello_prism.cpp) — the smallest server.
- [`examples/task_api.cpp`](examples/task_api.cpp) — an in-memory CRUD REST
  service: typed path/query/body parameters, JSON/YAML/CBOR content negotiation,
  a streamed `GET /tasks.csv` export, an async endpoint, a custom log sink, and an
  optional port read from `argv`.
- [`examples/per_thread_state.cpp`](examples/per_thread_state.cpp) — per-thread
  handler state across workers; takes a `workers` count on the command line.
- [`examples/upload_stream.cpp`](examples/upload_stream.cpp) — streaming request
  bodies (large uploads consumed without buffering) over HTTP/1.1 and TLS.
- [`examples/hello_h2c.cpp`](examples/hello_h2c.cpp) — cleartext HTTP/2 (h2c);
  try `curl --http2-prior-knowledge -v http://127.0.0.1:8080/health`.
- [`examples/hello_h2tls.cpp`](examples/hello_h2tls.cpp) — HTTP/2 over TLS with
  ALPN; takes `cert.pem key.pem` on the command line.
- [`examples/hello_websocket.cpp`](examples/hello_websocket.cpp) — a `/ws` echo
  endpoint plus a tiny browser page; also try `websocat ws://127.0.0.1:8080/ws`.
- [`examples/reverse_proxy.cpp`](examples/reverse_proxy.cpp) — a host-based reverse
  proxy (HTTP + WebSocket passthrough via `install()`) with routes given as
  `host=backend_host:port` on the command line.
- [`examples/webapp.cpp`](examples/webapp.cpp) — a REST API plus a static site
  (`examples/webroot/`) served from one prism app via `static_files`.
- [`examples/react-app/`](examples/react-app) — a TypeScript **React + Vite** app:
  hot-reload development against a live prism API, then deployed as one prism
  binary serving the built SPA and the REST API. See its
  [README](examples/react-app/README.md) for the dev/deploy workflow.

## License

MIT — see [LICENSE](LICENSE).
