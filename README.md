# prism

A lean REST service library for C++23, built on [vio](https://github.com/jorgen/vio)
(async I/O on libuv + coroutines + TLS) and [structify](https://github.com/jorgen/structify)
(zero-boilerplate JSON ↔ struct).

The name continues vio's light theme: libuv is *ultraviolet*, vio is *violet io*,
and a **prism** disperses one beam into a spectrum — exactly what an HTTP router
does with an incoming request stream.

## Design goals

- **Lean.** A small surface that leans on vio and structify rather than
  re-implementing I/O or serialization.
- **Easy.** Handlers are coroutines. Request and response bodies are plain C++
  structs with an `STFY_OBJ(...)` line — prism parses and serializes for you.
- **Consistent.** Errors flow through `prism::result_t<T>` (`std::expected<T,
  error_t>`), mirroring vio so the two libraries feel like one.

## A taste

```cpp
#include <prism/app.h>
#include <prism/json.h>
#include <vio/run.h>

struct user_t { std::string id, name; int age = 0; STFY_OBJ(id, name, age); };

int main() {
  return vio::run([](vio::event_loop_t &loop) -> vio::task_t<int> {
    prism::app_t app;

    app.get("/users/{id}", [](prism::request_t req) -> vio::task_t<prism::response_t> {
      user_t u{ std::string(req.param("id")), "Ada", 36 };
      co_return prism::json::respond(prism::status_t::ok, u);   // -> JSON body
    });

    co_await app.listen(loop, "0.0.0.0", 8080);
    co_return 0;
  });
}
```

## Status

Early scaffolding. In place and tested:

- `status_t`, `error_t`, `result_t` — status codes and error handling
- `request_t` / `response_t` — HTTP message types
- `json` — structify glue (`serialize`, `parse`, `respond`)
- `router_t` / `app_t` — route registration with `{param}` capture and dispatch

Next milestone: the HTTP/1.1 server codec behind `app_t::listen` (parse requests
off a vio TCP/TLS connection, run them through the router, write responses back).

## Building

```bash
cmake --preset debug
cmake --build cmake-build-debug
ctest --preset debug          # or: ./cmake-build-debug/tests/prism_tests
./cmake-build-debug/examples/hello_prism
```

Dependencies (vio, structify, doctest) are fetched automatically at configure
time via [cmake-dep](https://github.com/jorgen/cmake-dep).

### Sanitizers

`asan`, `tsan`, `ubsan`, and `msan` presets are available, e.g.:

```bash
cmake --preset asan && cmake --build cmake-build-asan && ctest --preset asan
```
