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
- **HTTP/1.1 server** — keep-alive, request pipelining, and slow-loris hardening
  (idle / header / body / write timeouts, `max_requests`, `max_connections`),
  parsing with [llhttp](https://github.com/nodejs/llhttp).
- **Typed route parameters** — bind `path_t<"id", int>`,
  `query_t<"q", std::optional<int>>`, and `body_t<T>` (parsed by `Content-Type`)
  straight into the handler signature; prism parses, validates, and answers `400`
  *before* your code runs.
- **JSON / YAML / CBOR — negotiated** — request/response bodies are plain structs
  with a single `STFY_OBJ(...)` line; prism picks the response format from the
  `Accept` header (`406` if it can't) and parses the request body by its
  `Content-Type` (`415` / `400`).
- **Serve finished bytes** — hand prism an already-serialised or binary payload
  (an image, a proxied body, a hand-built CSV) with
  `response_t::finished(status, content_type, bytes)`; read raw uploads with
  `request.raw_body()`.
- **Pluggable logging** — a `std::function` sink with a level filter; stdout by
  default, swap in your framework in one line.
- **Lean & consistent** — errors flow through `result_t<T>`
  (`std::expected<T, error_t>`), mirroring vio so the two libraries feel like one.
- Prism's own code is compiled `-fno-exceptions -fno-rtti`.

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
  std::string_view id = request.param("id");   // path parameter
  std::string q       = request.query("q");     // query parameter (percent-decoded, + -> space)
  bool verbose        = request.has_query("v");
  co_return prism::response_t::text(prism::status_t::ok, "ok");
}
```

## Content negotiation

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

## Async handlers

Handlers are coroutines. Take a `request_t` to reach the event loop and
`co_await` anything vio offers:

```cpp
vio::task_t<prism::response_t> slow(prism::path_t<"ms", int> ms, prism::request_t request)
{
  co_await vio::sleep(*request.loop, std::chrono::milliseconds{ms.value});
  co_return prism::response_t::text(prism::status_t::ok, "done");
}
```

## Logging

Each `app_t` owns a logger that defaults to a stdout sink (warnings/errors to
stderr) and logs one access line per request plus lifecycle events. Plug in your
own framework with a one-line sink:

```cpp
app.logger().set_level(prism::log_level_t::debug);
app.logger().set_sink([](prism::log_level_t level, std::string_view message) {
  my_framework::log(static_cast<int>(level), message);
});
```

## Server options

Keep-alive and hardening knobs are passed to `listen` / `prism::run`:

```cpp
prism::keepalive_options_t options;
options.idle_timeout    = std::chrono::seconds{30};
options.header_timeout  = std::chrono::seconds{10};
options.body_timeout    = std::chrono::seconds{30};
options.write_timeout   = std::chrono::seconds{30};
options.max_requests    = 1000;   // per connection (0 = unlimited)
options.max_connections = 1024;   // concurrent  (0 = unlimited)

co_return co_await prism::run(loop, "0.0.0.0", 8080, routes, options);
```

`prism::run` builds the app, runs your `configure(app_t&)` callback, and listens;
`VIO_MAIN(loop, argc, argv) { ... }` (from vio) supplies `main` and the event
loop. Prefer **free-function coroutine handlers** with state bound at
registration over capturing coroutine lambdas.

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

## Examples

- [`examples/hello_prism.cpp`](examples/hello_prism.cpp) — the smallest server.
- [`examples/task_api.cpp`](examples/task_api.cpp) — an in-memory CRUD REST
  service: typed path/query/body parameters, JSON/YAML/CBOR content negotiation,
  a `GET /tasks.csv` route serving hand-built bytes via `finished`, an async
  endpoint, a custom log sink, and an optional port read from `argv`.

## License

MIT — see [LICENSE](LICENSE).
