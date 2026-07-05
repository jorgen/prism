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
  use a `shared_ptr`). Classic `handler_t` registration is unchanged; the typed
  overload is SFINAE-constrained to non-`handler_t` callables.
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

### HTTP/2 (hand-rolled: h2c + h2 over TLS)

prism implements HTTP/2 from scratch — **no nghttp2 or other HTTP/2 dependency**;
the frame codec + HPACK are `detail/http2/`. Everything above the transport
(`request_t`/`response_t`/`router`/typed handlers/content-negotiation) is reused
unchanged: the seam is *build `request_t` → `router->dispatch` → encode
`response_t`*.

- **Enabling it**: h2c (cleartext) via `keepalive_options_t::protocol =
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
- **Streaming request bodies** — response streaming is done for both HTTP/1.1
  (chunked) and HTTP/2 (`response_t::streaming`); request bodies are still fully
  buffered (a streaming *inbound* body abstraction is future work).
- **Per-stream HTTP/2 timeouts** — the h2 read loop uses a coarse connection-level
  idle/header timeout; true per-stream header/body deadlines are future work.
