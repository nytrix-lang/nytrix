<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":200,"summary":"Compiling, testing, and debugging Nytrix for WebAssembly and modern web browsers."} -->
# Web & WebAssembly Guide

Nytrix supports compiling to standalone WebAssembly and executing directly in browser environments.

## Web Tooling & Commands

```bash
# Run the complete browser test suite
./make web-test

# Build WebAssembly demonstration bundles
./make web-demos

# Validate web runtime contracts
./make web-check

# Compile standalone WebAssembly binary
./make wasm
```

## Runtime Contracts

Nytrix provides two distinct WebAssembly compilation targets:

1. **`wasm-bare`**:
   - The minimal WebAssembly contract.
   - Embeds a compact runtime without requiring POSIX OS abstractions.
   - Ideal for embedded canvas, computational micro-tasks, and serverless edge functions.

2. **`wasm` (Full Runtime)**:
   - Full Nytrix runtime compiled via Emscripten.
   - Supports garbage collection, JIT compilation hooks, and virtualized filesystem operations (IndexedDB backed).
   - Requires Emscripten SDK (`emcc`).

## Browser Test Configuration (`tests.json`)

Browser test fixtures are declared in `etc/tests/native/web/tests.json`:

```json
{
  "engine": "wasm",
  "fixture": "etc/tests/native/web/canvas_basic.ny",
  "marker": "RENDER_OK",
  "virtual_time_budget_ms": 5000
}
```

### Key Configuration Fields:
- **`engine`**: Target engine (`wasm`, `wasm-bare`).
- **`fixture`**: Path to the executable test file.
- **`marker`**: Expected stdout/console marker confirming successful test execution.
- **`virtual_time_budget_ms`**: Virtual-time limit to ensure tests do not deadlock or timeout under headless browsers.

## Headless Browser Debugging

- Tests automatically run under headless Chrome/Chromium via Puppeteer/Playwright if installed.
- Fixtures requiring live window lifecycles declare capability requirements and skip gracefully when running headless.
- Diagnostic logs capture console errors, unhandled rejections, and WebGL context state.
