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

### Windows (MSVC)

From a Visual Studio developer shell (so `cl` and `ninja` are on `PATH`; run
`vcvars64.bat` first if needed), use the `debug-msvc` preset:

```bash
cmake --preset debug-msvc
cmake --build --preset debug-msvc
ctest --preset debug-msvc
```

The default preset pins ninja to a homebrew path, so the `debug` preset is
macOS-only; `debug-msvc` and `msvc-asan` are the Windows entry points. prism
builds `-fno-exceptions/-fno-rtti` (MSVC `/EHs-c- /GR- -D_HAS_EXCEPTIONS=0`)
with `/W4 /WX` on its own targets, and the whole suite passes with MSVC.

## Dependencies

Fetched at configure time via [cmake-dep](https://github.com/jorgen/cmake-dep).
Pinned in `CMake/3rdPartyPackages.cmake` (bare `CmDepFetchPackage(name version url
SHA256=…)` calls), then added via `CmDepAddPackage` in `CMake/Build3rdParty.cmake`:

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

### Togglable dependencies (pre-built / find_package)

The per-dependency knobs are **auto-declared by cmake-dep** (from each
`CmDepFetchPackage` call), prefixed by the project name (uppercased `PROJECT_NAME`
→ `PRISM`): the toggles `PRISM_USE_SYSTEM_VIO`, `PRISM_USE_SYSTEM_STRUCTIFY`,
`PRISM_USE_SYSTEM_DOCTEST`, `PRISM_USE_SYSTEM_LLHTTP` (default `OFF` = bundled),
plus `PRISM_<DEP>_{VERSION,URL,SHA256}` cache overrides so any dep can be fetched
at a different version/URL/hash without editing the packages file. When a toggle
is `ON`, cmake-dep skips the fetch and `CmDepAddPackage` calls
`find_package(<dep> CONFIG REQUIRED)` instead of `add_subdirectory`. All targets
are linked by their **namespaced** name (`vio::vio`, `structify::structify`,
`doctest::doctest`, `llhttp::llhttp`) so the same link line works bundled or
system — bundled vio adds a `vio::vio` alias. (Watch for bare target names:
`examples/CMakeLists.txt` and `tests/CMakeLists.txt` must use `vio::vio` /
`doctest::doctest`, not `vio` / `doctest`, or the system build fails to link.)

The prefix comes from `PROJECT_NAME` (which `project()` re-scopes per subtree), so
bundled vio's own dependencies are declared as `VIO_*` knobs, not `PRISM_*` — the
two families coexist in one cache.

- **vio** (`PRISM_USE_SYSTEM_VIO`): requires a vio installed with `VIO_INSTALL=ON`
  **and built against system deps** — vio only emits a relocatable
  `find_package(vio)` config in that configuration (see vio's `CMake/vioConfig.cmake.in`,
  which `find_dependency`s libuv/LibreSSL/ada).
- **Overriding vio's deps through bundled vio**: prism drives bundled vio via
  `CmDepAddPackage(vio … OPTIONS VIO_BUILD_*=OFF VIO_INSTALL=OFF)`, forcing only
  the build knobs, never the `VIO_USE_SYSTEM_*` toggles — so a consumer can pass
  `-DVIO_USE_SYSTEM_LIBRESSL=ON` (etc.) or the `-DVIO_<DEP>_VERSION/URL/SHA256`
  overrides straight through on the top-level configure line. Documented in the
  README "Dependencies & overrides" section.

The override/toggle machinery lives entirely in **cmake-dep**:
`CmDepFetchPackage` auto-declares `<PREFIX>_<DEP>_{VERSION,URL,SHA256}` +
`option(<PREFIX>_USE_SYSTEM_<DEP>)` and gates the fetch; `CmDepAddPackage` owns the
find-vs-build branch (`CONFIG`, `OPTIONS` build knobs, `PUBLIC_INCLUDE`,
`SKIP_IF_TARGET`, `NO_SYSTEM_FIND`) reading the `${name}_USE_SYSTEM` signal. vio
consumes the exact same mechanism with its own `VIO_` prefix.

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
    `{param}` capture, a trailing `{name...}` wildcard that captures the rest of
    the path, and `dispatch`).
  - `static_files.h` / `static_files.cpp` — `static_file_handler(root, index,
    spa_fallback)` (async file serving via vio, content-type by extension,
    index.html, traversal rejection, and an optional SPA fallback that serves
    index.html for extension-less navigation paths) and `content_type_for_path`;
    `app_t::static_files(prefix, root, spa_fallback)` mounts it on a
    `"<prefix>/{path...}"` wildcard route.
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
    - `server.h` / `server.cpp` — `serve` (cancellable TCP accept loop) and
      `serve_connection` (per-connection coroutine: read → feed → dispatch →
      write, with keep-alive and pipelining), plus `serve_tls` (TLS accept loop
      that finishes the handshake, reads the negotiated ALPN token, and routes to
      the h2 or HTTP/1.1 driver). Both HTTP/1.1 and h2 per-connection drivers are
      templated on a transport (`serve_connection_impl<Transport>`), so the same
      code runs over plain TCP and TLS.
    - `io_timeout.h` — shared transport layer: `read_outcome_t`/`write_outcome_t`,
      the templated `read_with_timeout<Reader>` watchdog, and `tcp_transport_t` /
      `tls_transport_t` (each exposes `start_reader()` / `read()` / `write()` over
      vio's plain-TCP or TLS socket API). Both read and write are timeout-bounded
      on both transports via a `cancellation_t` + watchdog `vio::sleep`; a
      timed-out write resolves `vio_cancelled` and is treated as fatal (the driver
      closes the connection, never retries — a cancelled TLS write may leave a
      partial record on the wire).
    - `http2/` — the hand-rolled HTTP/2 stack (no external HTTP/2 dep):
      - `frame.h` / `frame.cpp` — binary frame codec: `frame_reader_t`
        (incremental), per-type parse/serialize, the connection preface.
      - `hpack.h` / `hpack.cpp` — HPACK (RFC 7541): `hpack_decoder_t` (static +
        dynamic table, canonical Huffman generated from bit-lengths) and a simple
        stateless `hpack_encoder_t`.
      - `connection.h` / `connection.cpp` — `connection_t`, the transport-agnostic
        stream state machine + flow control: `receive(bytes) → ready_request_t`s,
        `submit_response`, `take_output` (flow-controlled frame emission). Holds
        the DoS hardening caps.
      - `server.h` / `server.cpp` — the async driver: read loop + single-flight
        coalescing writer + per-stream detached dispatch, over TCP
        (`serve_connection_h2`) or TLS (`serve_connection_h2_tls`).
- `tests/` — doctest (`DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS`).
  Coroutine handlers are exercised via `vio::run`, which runs the event loop and
  stops it automatically. HTTP/2 has unit tests (`http2_frame_tests`,
  `http2_hpack_tests` — incl. RFC 7541 Appendix C vectors, `http2_connection_tests`
  — incl. DoS-hardening) and end-to-end tests (`http2_server_tests`, an in-process
  h2 client over a loopback socket).
- `examples/` — `hello_prism.cpp` (minimal), `task_api.cpp` (in-memory CRUD REST
  service; its `/tasks.csv` export uses `response_t::streaming`), `hello_h2c.cpp`
  (cleartext HTTP/2, `listen` with `protocol = h2c`), `hello_h2tls.cpp` (HTTP/2
  over TLS with ALPN via `listen_tls`, cert/key from `argv`), `webapp.cpp` +
  `webroot/` (REST + a static site via `static_files`), and `react-app/` (a
  TypeScript React + Vite app: `server.cpp` serves the REST API and the built SPA
  with `spa_fallback`; `README.md` covers the dev-with-Vite-proxy / deploy
  workflow — `node_modules`/`dist` are git-ignored).

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
- **Typed handlers** (`params.h`): instead of taking `request_t` and parsing by
  hand, a handler can take typed extractors and register with
  `app.get(pattern, handler, bound_state...)` — `path_t<"id", int>`,
  `query_t<"q", std::optional<int>>` (optional ⇒ absent is allowed; non-optional
  ⇒ 400 when missing), `body_t<T>` (JSON via structify), and plain `request_t`
  for raw access (e.g. `request.loop`). The registration adapter parses each via
  the `param_codec_t<T>` customization point (built-ins for integral / bool /
  `std::string`; specialize it for your own types) and returns **400 before the
  handler runs** on any parse failure. Bound state args come first, extractors
  after. Contract: extractor / `request_t` params are taken **by value** (a
  reference qualifier is a static_assert), their value types are
  default-constructible, and bound state must be copyable (reused per request —
  use a `shared_ptr`, or `std::ref`/`std::cref` to bind a real reference to
  app-owned state, const or not). Classic `handler_t` registration is unchanged;
  the typed overload is SFINAE-constrained to non-`handler_t` callables.
- **Per-thread state** (`per_thread<T>`): register a factory once on the app with
  `app.provide_per_thread<T>(factory)`; a handler then takes a `prism::per_thread<T>`
  extractor and gets a **mutable `T&`** (`*`/`->`/`get()`) to *this thread's*
  instance. One instance per thread (keyed by `request.loop`, the per-thread
  identity — no `thread_local`), **shared by every handler that takes
  `per_thread<T>`** (the factory is type-keyed on the app, so N handlers → one
  instance per thread, not N). The registry (`detail::per_thread_registry_t`,
  `detail/thread_state.h`: type-keyed `per_thread_storage_t<T>` = factory +
  `shared_mutex` + `map<event_loop_t*, unique_ptr<T>>`) rides on `router_t` as a
  `shared_ptr`; `router_t::dispatch` stamps `request.factories` before running the
  handler, so the extractor resolves it with no server/`app.cpp` plumbing. A
  `per_thread<T>` for an unregistered `T` → 500. NB: prism is single-threaded
  today (one loop), so this is one instance now; it becomes genuinely
  one-per-thread once a multi-loop/thread runtime is added. Register factories
  before `listen()`.
- **Startup route verification**: registering a typed handler checks its
  compile-time `path_t<"name">` extractors against the runtime pattern; a handler
  binding a `{name}` the pattern does not declare (e.g. `path_t<"id">` against
  `"/x/{ide}"`) is recorded in `app_t::route_errors()` and makes `listen()` (and
  `prism::run`) fail fast with an `internal_server_error` before binding, naming
  the offending route and parameter.
- **Static (compile-time) route verification** (opt-in): registering with the
  pattern as an NTTP — `app.get<"/tasks/{id}">(handler, state...)` — runs the same
  `path_t` ↔ `{name}` check via a `constexpr pattern_declares_param` +
  `static_assert` (`detail::verify_routes_static`), so a mismatch is a **build
  error** that names the route and parameter (through the
  `require_declared_param_t<Pattern, Name>` instantiation). It coexists with the
  runtime form: `get<Pattern>(...)` (explicit NTTP) selects the static overload;
  `get(pattern, ...)` (runtime `string_view`) selects the runtime one, which can
  still register after `listen` has started. Both are constrained to non-`handler_t`
  callables.
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

**Bind address & dual-stack**: `listen` / `listen_tls` resolve the `host`
argument by family — an IPv6 literal (`"::1"`, `"::"`) binds IPv6, anything else
IPv4. An **empty host is the default and binds `::` as dual-stack** (libuv clears
`IPV6_V6ONLY` when `UV_TCP_IPV6ONLY` is absent, on both Windows and Linux, so one
socket accepts IPv6 *and* IPv4-mapped clients), falling back to `0.0.0.0` if the
`::` bind fails (IPv6 unavailable). This is why `localhost` works out of the box:
Node/browsers resolve it to `::1`, which an IPv4-only (`0.0.0.0`/`127.0.0.1`)
listener refuses. vio already exposes `ip6_addr` + `tcp_bind(flags)`; the family
logic is `bind_one`/`resolve_and_bind` in `app.cpp` (no vio change needed). Tested
in `server_tests.cpp` ("a dual-stack listener on :: accepts both IPv4 and IPv6").

**Keep-alive hardening** (`server_options_t` in `server_options.h`, passed to
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

### Multi-worker runtime (SO_REUSEPORT)

`server_options_t::worker_threads` scales the server across cores. It **defaults
to `0` = `hardware_concurrency()`** (floor 1), so prism is multi-threaded out of
the box; `1` forces the single-loop model exactly. Any value runs that many event
loops on that many threads — the caller loop is **worker 0**; prism spawns the
rest via `vio::thread_with_event_loop_t` — each binding the *same* port with
`UV_TCP_REUSEPORT` (SO_REUSEPORT), so the kernel load-balances accepts. Because `per_thread<T>` is keyed by
`request.loop`, one worker per thread ⇒ one instance per thread, no code change.
This is entirely prism-side: `vio::tcp_bind` already forwards `uv_tcp_bind` flags,
so no vio change was needed. The family/dual-stack decision is made once in
`resolve_and_bind` (returns `bound_server_t::ipv6`); workers **replay worker 0's
resolved `{host, ipv6}`** so every loop binds the same address (no independent
`::`→`0.0.0.0` re-decision). `max_connections` is **per worker** (process ceiling
= `worker_threads * max_connections`).

Shutdown is a **graceful drain**. Each worker owns its own lock-free
`vio::cancellation_t` (`cancellation_t` has single-thread affinity — only ever
`cancel()`/`register_callback`/`~registration_t` a token on its owning loop
thread). The user's `cancel` (fired on the caller thread) fans out via
`cancel->register_callback` → `run_in_loop([c]{ c->cancel(); })` onto each worker
loop, so every worker stops accepting on its own thread. `serve`/`serve_tls`, on
the cancel branch, `co_await drain_active(...)` — poll the per-loop `active`
counter (20 ms `vio::sleep`, cancel `nullptr`) until it hits 0 or
`options.shutdown_timeout` (default 10 s) elapses (warn-logged if it does) — then
return. `app_t::listen` awaits worker 0's `serve`, calls `stop_workers()` again
(covers the error path where worker 0 ended without the user cancelling), then a
`std::latch{N-1}` `wait()`s until every worker's detached serve counted down, and
finally `pool.clear()` (`~thread_with_event_loop_t` → `stop_and_join`) — join only
after all loops are idle, never force-stopped mid-drain. `worker_t`/`latch`/
`registration_t` live in the `listen` coroutine frame (alive across the
`co_await`); the router/logger `shared_ptr` snapshot is shared to all workers.
`listen_tls` mirrors this, each worker doing its own `ssl_server_create`
(⇒ its own `SSL_CTX`, N× context memory). TSan-clean; the single-worker path is
byte-identical to before. See `examples/per_thread_state.cpp` (takes a `workers`
argv).

### Logging

Logging is a pluggable, per-app facade (`logging.h`). `app_t` owns a
`std::shared_ptr<logger_t>` (default-constructed with `default_stdout_sink`);
configure it via `app.logger().set_sink(...)` / `set_level(...)` before
`listen`, or replace it wholesale with `app.set_logger(...)`. A `log_sink_t` is
just `std::function<void(log_level_t, std::string_view)>`, so bridging to spdlog
/ glog / etc. is a one-line lambda; the default sink writes `timestamp [LEVEL]
message` to stdout (warn/error to stderr) and **fflushes each line** so logs
survive a crash; it guards the write with a `std::mutex` (held in a captured
`shared_ptr`) so multi-worker output does not tear — a **custom sink must be
thread-safe** when `worker_threads > 1`. `logger_t::enabled(level)` gates formatting, and the per-request
access line is only built when `info` is enabled. The logger rides into the
connection coroutines as `std::shared_ptr<const logger_t>` exactly like the
router (`serve`/`serve_connection` take it; a null logger means silent — tests
pass `nullptr`). prism logs: `listen` start / stop and bind failures
(info/error), one access line per request (`info`: `METHOD path status NNNus`),
malformed requests / header-body-write timeouts / `max_connections` sheds
(`warn`), and connection accept / idle-close / read-error (`debug`).

### HTTP/2 (hand-rolled: h2c + h2 over TLS)

prism implements HTTP/2 from scratch — **no nghttp2 or other HTTP/2 dependency**;
the frame codec + HPACK are `detail/http2/`. Everything above the transport
(`request_t`/`response_t`/`router`/typed handlers/content-negotiation) is reused
unchanged: the seam is *build `request_t` → `router->dispatch` → encode
`response_t`*.

- **Enabling it**: h2c (cleartext) via `server_options_t::protocol =
  protocol_t::h2c` passed to `listen`. h2-over-TLS via `app.listen_tls(loop, host,
  port, ssl_config)` — the ALPN list defaults to `{"h2","http/1.1"}`;
  `serve_tls` finishes the handshake, reads `ssl_server_client_alpn_selected`, and
  routes `h2` to the h2 driver and everything else to the HTTP/1.1 driver (both
  over the TLS transport). h2c is prior-knowledge only (the fixed connection
  preface); no `Upgrade: h2c`.
- **Connection driver** (`detail/http2/server.cpp`): one `shared_ptr` connection
  context co-owned by the read loop + a single-flight coalescing **writer** + per
  stream detached dispatch coroutines. Because HTTP/2 interleaves all streams'
  frames into one serialized byte stream drained by exactly one writer, the vio
  single-in-flight-write constraint is satisfied naturally. Handlers run
  concurrently (`detached_task_t` per stream) and, on completion, `submit_response`
  + trigger a flush; a WINDOW_UPDATE from the read loop also triggers a flush so
  flow-control-blocked bodies resume. All coroutines take the context **by value
  (`shared_ptr`)** — the vio dangling-`this` rule — so a suspended handler keeps the
  session alive and a late completion after teardown drops cleanly.
- **Flow control**: connection + per-stream send windows honoured on emit (DATA
  chunked to `min(max_frame, stream_window, conn_window)`, deferred when a window
  is exhausted, resumed on WINDOW_UPDATE); the receive window is replenished per
  DATA frame.
- **Streaming responses**: a handler may return `response_t::streaming(status,
  content_type, body_source_t)` where `body_source_t =
  std::function<vio::task_t<body_chunk_t>()>` — the server pulls chunks until one
  reports `last`. Works on both transports (see below). For h2 the streaming pump
  runs in the per-stream handler task: pull a chunk → `push_stream_data` →
  flush → wait on a driver flow-gate (resumed after WINDOW_UPDATE/flush progress)
  until the buffer drains, giving true backpressure. Request bodies are still
  buffered.
- **Hardening** (caps in `h2_settings_t`, enforced in `connection_t`):
  `SETTINGS_MAX_CONCURRENT_STREAMS`, `max_header_list_size` (HPACK-bomb bound, also
  gates CONTINUATION accumulation), `max_body_bytes`, rapid-reset cap
  (CVE-2023-44487), and a SETTINGS-flood cap — each maps to a GOAWAY /
  `ENHANCE_YOUR_CALM` or `RST_STREAM`.
- **Conformance**: passes the full **h2spec** suite in both h2c and h2-over-TLS
  modes — **146/146** against a large endpoint (`-P /big`, the 20 KB route in the
  examples). Against a tiny endpoint (`-P /health`) h2spec reports 145 + 1
  *skipped*: test 6.9.2.2 (shrink SETTINGS_INITIAL_WINDOW_SIZE to make a stream
  window negative) needs a still-in-flight, window-limited response to set up, so
  a 2-byte body makes h2spec skip it — not a failure, and it passes against `/big`.

### Streaming request bodies (opt-in, both transports)

Large inbound bodies can be **streamed** instead of buffered whole. Register with
`app.post_stream(pattern, handler)` (and `get_/put_/patch_/del_stream`): the route
is flagged `streaming` on the `router_t`, the server resolves it at
**headers-complete** (`router_t::resolve` / `is_streaming`, matching without
running the handler), dispatches the handler early, and hands it a pull-based
`request_t::body_reader` (`std::shared_ptr<request_body_t>`) instead of a filled
`request_t::body`. The reader (`http.h`) mirrors the output `body_source_t`:
- `co_await read_chunk()` — owned `body_chunk_t` (`{data, last}`), `last` at end;
- `co_await read_into(std::span<std::byte>)` — fills the handler's buffer, returns
  bytes (0 = EOF). For identity (Content-Length) bodies it reads **directly into
  the caller buffer**, bounded by the bytes still expected: **zero-copy on plain
  TCP** (vio's `tcp_reader_t::read_into`), and on TLS it `SSL_read`s straight into
  the buffer (vio's exact-fill `reader.read` — the fewest copies TLS allows, since
  libuv only ever sees ciphertext). Chunked bodies use the buffered path (llhttp
  must de-chunk);
- `co_await read_all()` — drain to a `std::string`;
- `length()` (Content-Length, `nullopt` if chunked), `at_end()`, `status()`
  (`body_read_status_t`: errors surface here, not thrown).

Streaming routes take a raw `request_t` (typed `body_t<T>` stays buffered-only).

- **HTTP/1.1** (`detail/http1.*`, `detail/server.cpp`): llhttp stays authoritative
  (framing/keep-alive/pipelining). The codec gains a streaming mode
  (`begin_streaming_body`/`take_header_request`/`take_body_into`/`take_body_chunk`/
  `message_complete`); `http1_body_reader_t<Transport>` drives further
  `transport.read`/`read_into` and `pause()`/`resume()` (real backpressure — libuv
  isn't left reading ahead). After the handler returns, `serve_connection_impl`
  drains the remainder up to `max_drain_bytes` (else closes) to keep the
  connection alive. The reader holds raw codec/transport pointers valid only for
  the inline `dispatch` — a streaming handler must not move `body_reader` into a
  task outliving `dispatch`.
- **HTTP/2** (`detail/http2/`): a streaming route is delivered at **END_HEADERS**
  (before END_STREAM) with `ready_request_t::streaming`; DATA frames queue into
  `stream_t::inbound_chunks` and the receive window is **consume-driven** — the
  reader (`request_body_h2_t`) calls `connection_t::inbound_consume` (emit
  WINDOW_UPDATE + flush) only as the handler takes bytes, so an unconsumed body is
  bounded by the advertised window (backpressure). The reader parks on an
  `inbound_gate_t` and is resumed by the driver via `collect_inbound_ready` after
  each `receive`; an undrained stream is `RST_STREAM(NO_ERROR)`'d after the
  response. The **buffered DATA path is kept byte-identical** (immediate
  WINDOW_UPDATE) so h2spec conformance is unaffected.

Caps are configurable in `server_options_t`: `max_header_bytes` (default 64 KiB;
h1 431, h2 `max_header_list_size`), `max_body_bytes` (buffered 413),
`max_streaming_body_bytes` (bounds a streaming reader's buffering; 0 = unbounded),
`max_drain_bytes`. Example: `examples/upload_stream.cpp`.

Depends on vio's `tcp_reader_t::read_into` (zero-copy scatter read) and
`pause()`/`resume()` (transient `uv_read_stop`/`start` for backpressure).

### Conformance & hardening testing (tools)

- **h2spec** (`github.com/summerwind/h2spec`, prebuilt Windows binary
  `h2spec_windows_amd64.zip`): the RFC 7540/9113 + RFC 7541 conformance suite
  (~146 cases). h2c is the default; add `-t -k` for TLS+ALPN. Run against a running
  example: `h2spec -h 127.0.0.1 -p 8080 -P /big` (h2c) or
  `h2spec -t -k -h 127.0.0.1 -p 8443 -P /big` (TLS) — use a large endpoint
  (`/big`) so the flow-control tests have a window-limited response to exercise
  (`/health` makes 6.9.2.2 skip). This is the primary gate; it is cross-process,
  which is also how TLS is validated end-to-end (an in-process same-loop TLS
  client+server deadlocks on large-transfer backpressure, so there is no in-suite
  TLS test — h2spec `-t` and `curl -k https://…` cover it).
- **hpack-test-case** (`github.com/http2jp/hpack-test-case`) and
  **http2-frame-test-case** — portable JSON wire-vector fixtures; the HPACK decoder
  is also unit-tested against RFC 7541 Appendix C vectors directly.
- **h2load** (nghttp2; WSL/Docker on Windows) — load/soak with many concurrent
  streams: `h2load -n 100000 -c 100 -m 100 http://127.0.0.1:8080/`.
- **DoS regression** — Rapid Reset (`secengjeff/rapidresetclient`), CONTINUATION
  flood (CERT VU#421644), HPACK bomb — map onto the `h2_settings_t` caps above.
- **Sanitizers** — the whole suite (incl. the concurrent h2 driver) is ASan-clean
  via the `msvc-asan` preset.

### Hardening backlog

Remaining:
- **`408 Request Timeout` response** — a mid-request HTTP/1.1 header/body timeout
  currently just closes the connection; it could instead emit a `408` before
  closing.
- **Per-stream HTTP/2 timeouts** — the h2 read loop uses a coarse connection-level
  idle/header timeout; true per-stream header/body deadlines are future work.
- **Streaming request bodies via typed handlers** — streaming routes take a raw
  `request_t`; `body_t<T>` remains buffered-only (a future async `body_t` could
  `co_await read_all()`).
