# prism + React + Vite

A TypeScript React app (bundled by [Vite](https://vitejs.dev/)) whose REST API and
production static assets are both served by **prism**. It shows the two workflows
you actually use:

- **Develop** with Vite's dev server (instant hot-module reload) talking to a
  live prism API.
- **Deploy** as a single prism binary that serves the built SPA *and* the API.

```
examples/react-app/
├─ index.html, src/…        the React + TS app
├─ vite.config.ts           dev server + /api proxy + build output
├─ package.json             react, vite, typescript
├─ server.cpp               the prism server (REST API + static SPA)
└─ dist/                    created by `npm run build` (git-ignored)
```

The API is a tiny in-memory task list: `GET /api/tasks`, `POST /api/tasks`
(`{"title": …}`), `PUT /api/tasks/{id}` (toggle done).

## Prerequisites

- Node.js 18+ and npm.
- The `react_server` example built (from the repo root):
  ```bash
  cmake --preset debug           # or debug-msvc on Windows
  cmake --build cmake-build-debug --target react_server
  ```
- Install the frontend dependencies once:
  ```bash
  cd examples/react-app
  npm install
  ```

## Develop (hot reload)

Two processes. **Terminal 1** — the prism API on port 8080:

```bash
./cmake-build-debug/examples/react_server           # dist need not exist yet
```

**Terminal 2** — Vite's dev server on port 5173:

```bash
cd examples/react-app
npm run dev
```

Open **http://localhost:5173**. Vite serves the app with hot-module reload; every
request to `/api/*` is proxied to the prism server on 8080 (see the `proxy` block
in `vite.config.ts`), so you edit the UI and hit the real API at once. Only the
API routes actually reach prism in this mode — Vite serves the HTML/JS/CSS.

## Deploy (one server)

Build the app into static assets, then let prism serve everything from one port:

```bash
cd examples/react-app
npm run build                                        # -> examples/react-app/dist

# from the repo root:
./cmake-build-debug/examples/react_server examples/react-app/dist 8080
```

Open **http://localhost:8080**. Now a single prism process serves the built SPA
*and* the REST API — no Node, no second server. `react_server` takes the dist
directory and port as arguments (defaults: `dist` and `8080`).

## How it fits together

- `server.cpp` registers the REST routes **first**, then mounts the SPA:
  ```cpp
  app.get("/api/tasks", list_tasks, store);
  app.post("/api/tasks", create_task, store);
  app.put("/api/tasks/{id}", toggle_task, store);
  app.static_files("/", dist, /*spa_fallback=*/true);   // catch-all, last
  ```
  The router matches in registration order, so `/api/*` wins over the static mount.
- `spa_fallback = true` makes prism return `index.html` for unmatched **navigation**
  paths (e.g. `/dashboard`), so client-side routing (React Router) survives a full
  page reload — while a missing *asset* (`/assets/x.js`) still returns 404.
- Content types come from the file extension; `..` path traversal is rejected.
- The server binds with an **empty host**, i.e. dual-stack `::` — it accepts IPv4
  (`127.0.0.1`) and IPv6 (`::1`) alike, so the browser's `http://localhost` and
  Vite's dev proxy both reach it regardless of how `localhost` resolves. The proxy
  in `vite.config.ts` targets `127.0.0.1` explicitly (the most robust choice — it
  works even against an IPv4-only server); with the dual-stack default, plain
  `localhost` works too. (Node resolves `localhost` to `::1` first and does *not*
  fall back, so a `localhost` proxy against an IPv4-only server would fail — hence
  both the explicit proxy target and the dual-stack bind.)
- In dev, the same `/api` contract is served by prism and reached through Vite's
  proxy, so dev and prod behave identically.

For HTTP/2 (h2c or h2 over TLS with ALPN), pass `server_options_t` /
`listen_tls` exactly as the other examples show — the React app doesn't change.
