# FlipDot Remote Console

Modern React frontend for controlling the ESP32 flip-dot firmware. The UI is optimised for mobile, offers a live canvas, and wraps the existing HTTP + WebSocket endpoints exposed by `main/web_server.c`.

## Scripts

```bash
npm install        # install dependencies
npm run dev        # start Vite dev server (default http://localhost:5173)
npm run build      # create production build in dist/
npm run preview    # preview the production build locally
npm run lint       # run ESLint (JSX/React rules)
```

## Feature Overview

- Connect/disconnect to `ws://<host>/ws`, auto-push framebuffer while streaming.
- Draw or erase on a 14×28 flip-dot canvas with round yellow dots, mobile-friendly gestures.
- Manual controls: clear, fill, and push the framebuffer on demand.
- Mode management: quick-select mirrored firmware modes, cycle next/prev, send scrolling text, trigger alerts, toggle invert.
- Status panel summarises connection state, last frame dispatch, and recent events.

## Configuration Notes

- The host/IP field accepts raw addresses or full URLs (e.g. `192.168.1.50`, `http://display.local:80`).
- HTTP commands use simple GET requests with `mode: 'no-cors'`, matching the ESP32 web server defaults.
- Raw framebuffer payloads are sent as 392-byte `Uint8Array` buffers (row-major order, non-zero = yellow dot).

## Embedding in the ESP32 firmware

The fastest path is `npm run deploy:esp32`, which runs a fresh build, copies the assets into `main/web_static`, and creates the pre-compressed files the firmware expects.

Manual steps (if you prefer):

1. Build the production bundle: `npm run build` (outputs to `client/dist`).
2. Copy the generated assets into `main/web_static`, renaming the bundled script to `assets/app.js` (e.g. `cp dist/assets/index-*.js main/web_static/assets/app.js`). The firmware build embeds the gzipped versions automatically via `main/CMakeLists.txt`.
3. Rebuild and flash the ESP32 firmware with `idf.py build flash monitor`.

The UI loads from `http://<esp32-ip>/`, and the default connection host is auto-filled from the browser origin so the WebSocket works without manual input.

## Project Structure

```
src/
  api/              // HTTP helpers targeting firmware endpoints
  features/         // UI modules (canvas, connection, mode, status)
  state/            // Zustand store managing connection + framebuffer
  theme/            // Chakra UI theme with dark styling
  lib/              // Shared constants + host utilities
```

The project uses React 18, Chakra UI 2, Zustand, and Vite for fast iteration.
