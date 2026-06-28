# prism

A lean REST service library for C++23, built on **vio** (async I/O: libuv +
coroutines + TLS) and **structify** (header-only JSON ↔ struct). Name theme:
libuv = ultraviolet, vio = violet io, prism = disperses a request stream into
routes.

## Build

```bash
cmake --preset debug
cmake --build cmake-build-debug
ctest --preset debug                       # or ./cmake-build-debug/tests/prism_tests
./cmake-build-debug/examples/hello_prism
```

Sanitizer presets: `asan`, `tsan`, `ubsan`, `msan` (see `CMakePresets.json`).

## Dependencies

Fetched at configure time via [cmake-dep](https://github.com/jorgen/cmake-dep).
Pinned in `CMake/3rdPartyPackages.cmake` (URL + SHA256), added as subdirectories
in `CMake/Build3rdParty.cmake`:

- **vio** — built static (`VIO_BUILD_SHARED OFF`). Its headers are exposed
  PRIVATE upstream, so `Build3rdParty.cmake` re-exposes `${vio_SOURCE_DIR}/src`
  as a PUBLIC include on the `vio` target. We own vio
  (`github.com/jorgen/vio`); the server's read/write timeouts rely on
  cancellation primitives added there — `write_tcp(…, cancellation_t*)` (owns the
  payload for the duration of a cancellable write so a cancelled in-flight
  `uv_write` is UAF-free; an owning overload takes any moved-in contiguous byte
  range — `std::string`, `std::vector`, … — and type-erases its lifetime via
  `owned_payload_t`, while the `const uint8_t*` overload copies) and
  `stream_reader_t::cancel()` for the TLS reader. Bumping vio = push to vio `master`, then update the commit in the URL
  **and** recompute the SHA256 here.
- **structify** — INTERFACE target `structify::structify`.
- **doctest** — testing.
- **llhttp** — Node.js's HTTP/1.1 parser (C). Built static
  (`LLHTTP_BUILD_STATIC_LIBS ON`, shared off), linked **PRIVATE** to prism and
  wrapped behind `detail/http1.h` (PIMPL) so `<llhttp.h>` never reaches a public
  header or prism's include interface. The pinned release tarball already ships
  the generated sources, so no Node toolchain is needed at build time.

To bump a dependency: change the git ref in the URL **and** recompute the
SHA256 (`curl -sL <url> | shasum -a 256`).

## Conventions

- **C++23**, built with `-fno-exceptions -fno-rtti` (MSVC equivalents) for
  prism's own code. `Build3rdParty.cmake` strips those flags around the
  third-party `add_subdirectory` calls that need exceptions, then restores them.
- vio's `task_t` coroutine calls `std::terminate()` on unhandled exception (no
  throw), so it is compatible with `-fno-exceptions`.
- Naming (enforced by `.clang-tidy`): types/enums/aliases are `lower_case` with
  a `_t` suffix (`status_t`, `request_t`, `result_t`); functions/variables are
  `lower_case`.
- Errors use `prism::result_t<T>` = `std::expected<T, prism::error_t>`, mirroring
  vio's `std::expected<T, vio::error_t>`. `error_t` carries an HTTP `status_t`
  plus a message.
- **No comments in code.** Do not add comments — let the code speak for itself
  through clear naming. This applies to all code: library, tests, and examples.

## Git

- **No Claude/Anthropic attribution.** Do not add the `Co-Authored-By` line, the
  "Generated with Claude Code" footer, or any other Claude/Anthropic attribution
  to commits, PR bodies, or other generated content.

## Layout

- `src/prism/` — the library (mostly headers; `*.cpp` for out-of-line bits).
  - `status.h` — `status_t` enum + `reason_phrase`.
  - `error.h` — `error_t`, `result_t`, `fail()`.
  - `http.h` / `http.cpp` — `method_t`, `headers_t`, `request_t`, `response_t`.
  - `json.h` — structify glue: `serialize`, `parse`, `respond`.
  - `logging.h` / `logging.cpp` — `log_level_t`, `log_sink_t`
    (`std::function<void(log_level_t, std::string_view)>`), `logger_t`, and
    `default_stdout_sink`.
  - `router.h` / `router.cpp` — `handler_t`, `router_t` (segment matching,
    `{param}` capture, `dispatch`).
  - `app.h` / `app.cpp` — `app_t` facade; `listen()` binds a TCP server and
    drives connections through the router. `prism::run(loop, host, port,
    configure, options)` is a coroutine that builds an `app_t`, runs the
    `configure(app_t&)` callback, and `listen`s — pair it with vio's
    `VIO_MAIN(loop, argc, argv)` macro (which generates `main` and forwards the
    args into a `task_t<int>` coroutine body) for a boilerplate-free entry point.
  - `prism.h` — umbrella header + `version()`.
  - `detail/` — internal, non-installed headers (not on the public surface).
    - `http1.h` / `http1.cpp` — `request_codec_t`, a PIMPL wrapper over llhttp
      that turns a byte stream into `request_t`s (incremental, keep-alive,
      header/body size guards), plus `serialize_response` (`response_t` → wire,
      with authoritative `Content-Length`/`Connection`/`Date`).
    - `server.h` / `server.cpp` — `serve` (cancellable accept loop) and
      `serve_connection` (per-connection coroutine: read → feed → dispatch →
      write, with keep-alive and pipelining).
- `tests/` — doctest (`DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS`).
  Coroutine handlers are exercised via `vio::run`, which runs the event loop and
  stops it automatically.
- `examples/` — `hello_prism.cpp` (minimal) and `task_api.cpp` (an in-memory CRUD
  REST service showing free-function handlers, JSON binding, an async handler, a
  custom log sink, an optional port read from `argv`, and the
  `VIO_MAIN(loop, argc, argv)` + `prism::run` entry point).

## Architecture notes

- **Handlers are coroutines**: `handler_t = std::function<vio::task_t<response_t>(request_t)>`.
  Because they return `task_t`, a handler can `co_await` any async vio operation.
  The event loop is reachable as `request_t::loop` (set by the server before
  dispatch), so a handler can `co_await vio::sleep(*request.loop, …)` without
  capturing it. Prefer **free-function coroutines that take their dependencies by
  parameter** (registered with `std::bind_front(fn, deps...)`) over capturing
  coroutine lambdas — a coroutine lambda's captures live in the closure, which is
  the vio dangling-`this` footgun; a free function takes its state into the frame
  by value.
- **Routing**: `router_t` splits patterns and paths into `/`-segments and matches
  segment by segment; a `{name}` segment captures into `request_t::params`.
  `dispatch` yields 404 (no path match) or 405 (path matches, method doesn't).
- **Path vs query params**: `{name}` in a pattern captures a path *segment* into
  `request_t::params` (read via `request.param("name")`) and participates in
  routing. Query-string parameters are a separate, never-declared concept — read
  any of them with `request.query("name")` (percent-decoded, `+`→space, returns a
  `std::string`) or test presence with `request.has_query("name")`. Both scan
  `request.target` on demand; routing ignores the query string entirely.
- **Body binding**: request/response bodies are structs with `STFY_OBJ(...)`.
  `json::parse<T>` returns `result_t<T>` (400 on malformed input); `json::respond`
  serializes a struct and sets `Content-Type: application/json`.

### HTTP/1.1 server — `app_t::listen`

`listen()` binds a vio TCP server and runs an accept loop (`detail::serve`).
Each connection is handled by a detached `detail::serve_connection` coroutine
that reads off the socket, feeds bytes to a per-connection `request_codec_t`
(llhttp), `co_await _router.dispatch(...)`s each parsed request, and writes the
serialized `response_t` back — with HTTP/1.1 keep-alive and request pipelining.
Connection handlers are detached `vio::detached_task_t`s, so they free their own
frame on completion; passing a `vio::cancellation_t*` stops the accept loop for
graceful shutdown. The full suite (incl. an end-to-end server test) is ASan- and
TSan-clean. Mind vio's documented `socket_stream` re-entrancy guards (a write
completion can destroy the stream) when touching the connection loop.

**Keep-alive hardening** (`keepalive_options_t` in `server_options.h`, passed to
`listen`): `idle_timeout` bounds the wait between requests; `header_timeout` and
`body_timeout` are `steady_clock` absolute deadlines (so a byte-trickle slow-loris
cannot reset them) — the budget switches from header to body the moment the codec
reports `current_headers_complete()`. Each connection-loop iteration re-seeds that
budget from the codec's in-progress state (`current_in_progress()` /
`current_headers_complete()`), so a pipelined request whose bytes already arrived
in the previous request's TCP segment keeps the header/body budget instead of
falling back to `idle_timeout` (a stalled pipelined body would otherwise dodge the
body timeout). `max_requests` caps requests per connection,
after which the server forces `Connection: close`; `max_connections` caps
concurrent connections (`serve` sheds accepts past the cap via a shared counter
the per-connection task decrements on exit). A value of `0` disables the relevant
timeout/cap.
Each timed read is bounded by a named **watchdog**: an immediately-invoked
`task_t` (state passed by parameter, never captured) that `co_await`s a
cancellable `vio::sleep` and calls `reader.cancel()` on expiry. When the read
wins, the connection does `token.cancel()` then `co_await std::move(watchdog)` —
both **stop and settle** are mandatory on every exit path; this is UAF-free
because `vio::sleep` resets its `cancel_registration` on both the fire and cancel
paths before the token is destroyed (verified against `sleep.h`).

The same watchdog bounds the response **write** (`write_timeout`): `write_tcp`
takes the cancel token, so on a slow-reading client the write resolves cancelled
and the connection closes — UAF-free because vio owns the bytes for the write's
duration. prism `std::move`s the serialized `wire` into vio's owning
`write_tcp` overload, so the in-flight `uv_write` reads vio-owned storage, not
prism's freed buffer, and without a copy. Routing state is shared via
`std::shared_ptr<const router_t>` (a copy of the router taken at `listen`), so
in-flight connection coroutines keep it alive even if the `app_t` is destroyed.

### Logging

Logging is a pluggable, per-app facade (`logging.h`). `app_t` owns a
`std::shared_ptr<logger_t>` (default-constructed with `default_stdout_sink`);
configure it via `app.logger().set_sink(...)` / `set_level(...)` before
`listen`, or replace it wholesale with `app.set_logger(...)`. A `log_sink_t` is
just `std::function<void(log_level_t, std::string_view)>`, so bridging to spdlog
/ glog / etc. is a one-line lambda; the default sink writes `timestamp [LEVEL]
message` to stdout (warn/error to stderr) and **fflushes each line** so logs
survive a crash. `logger_t::enabled(level)` gates formatting, and the per-request
access line is only built when `info` is enabled. The logger rides into the
connection coroutines as `std::shared_ptr<const logger_t>` exactly like the
router (`serve`/`serve_connection` take it; a null logger means silent — tests
pass `nullptr`). prism logs: `listen` start / stop and bind failures
(info/error), one access line per request (`info`: `METHOD path status NNNus`),
malformed requests / header-body-write timeouts / `max_connections` sheds
(`warn`), and connection accept / idle-close / read-error (`debug`).

### Hardening backlog

Write/dispatch timeout, graceful drain, max concurrent connections, the
header/body timeout split, and the zero-copy (move-in) cancellable write are all
done — they drove the vio cancellation work above. Remaining:
- **`408 Request Timeout` response** — a mid-request header/body timeout currently
  just closes the connection; it could instead emit a `408` before closing.
- **TLS write timeout** — see the TLS milestone below; vio's `socket_stream`
  write path has no cancellation yet.

### Next milestone — TLS

The codec is transport-agnostic; only the I/O calls differ. Adding TLS means a
`serve_connection`/`serve` variant over vio's `ssl_server_*` API (cert/key via
`vio::ssl_config_t`, `ssl_server_client_write` takes a `uv_buf_t`) — the parse
and serialize paths in `detail/http1.*` are reused unchanged. vio's TLS reader
(`stream_reader_t`) now has `cancel()`, so the read-timeout watchdog extends to
TLS as-is; the write-timeout will need the same cancellation on vio's TLS write
path (`socket_stream` write), which does not exist yet.
