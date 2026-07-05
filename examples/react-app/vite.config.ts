import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// During `npm run dev` the app runs on http://localhost:5173 with hot-module
// reload, and requests to /api are proxied to the prism server on :8080 — so
// you develop the UI against the real REST API. `npm run build` emits static
// assets into dist/, which the prism server serves in production.
export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    proxy: {
      "/api": "http://localhost:8080",
    },
  },
  build: {
    outDir: "dist",
  },
});
