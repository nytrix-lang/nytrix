(() => {
  const demos = Array.isArray(window.NYTRIX_WEB_DEMOS) ? window.NYTRIX_WEB_DEMOS : [];
  const $ = (id) => document.getElementById(id);
  const canvas = $("glCanvas");
  const cliStageOutput = $("cliStageOutput");
  const out = $("outputLog");
  const demoItems = $("demoItems");
  const controls = $("controlGrid");
  const wasmFile = $("wasmFile");
  const runArgsInput = $("runArgs");
  const runEntryInput = $("runEntry");
  const autoRunInput = $("autoRun");
  const mirrorOutputInput = $("mirrorOutput");
  const stage = document.createElement("canvas");
  const ctx = stage.getContext("2d", { alpha: false, willReadFrequently: true });
  const dec = new TextDecoder();
  const enc = new TextEncoder();
  const ENTRY_ORDER = ["ny_web_frame", "ny_web_render", "ny_web_main", "main", "_ny_top_entry"];
  const NATIVE_ENTRY = "_ny_top_entry";
  const NY_TRUE = 8n;
  const NY_FALSE = 2n;

  let gl = null;
  let program = null;
  let tex = null;
  let webgl3d = null;
  let currentMeta = null;
  let currentRuntime = null;
  let selectedArea = "All";
  let running = true;
  let lastTime = 0;
  let fpsTick = 0;
  let fpsFrames = 0;
  let presentCount = 0;
  let frameTouched = false;
  let framePresented = false;
  let runtimeToken = 0;
  let outputLines = [];
  let stdoutLine = "";
  let runArgv = ["ny"];
  let webFrameDt = 1 / 60;
  let assetFonts = new Map();
  let assetCache = new Map();
  let loadedAssetCount = 0;
  let nextFontId = 1;
  let g2dTextures = new Map();
  let nextTextureId = 1;
  let audioContext = null;
  let audioUnavailable = false;
  const fallbackMemory = new WebAssembly.Memory({ initial: 256, maximum: 1024 });
  const input = {
    key: "-", codes: new Set(), pressed: new Set(),
    mouse: [0, 0], buttons: new Set(), pressedButtons: new Set(), scroll: [0, 0],
    /* touches: Map<touchId, [x, y]> in logical stage coordinates */
    touches: new Map(),
    /* startedThisFrame: Set<touchId> for one-shot edge queries */
    touchStarts: new Set(),
  };
  /* Standard Gamepad mapping (https://w3c.github.io/gamepad/): buttons 0..15
     follow the standard layout (0 = bottom "A", 1 = right "B", ...) and
     axes 0..3 are left/right stick X/Y. `readGamepads` honours an injected
     fake set for headless self-tests (no real device exists under --dump-dom). */
  function readGamepads() {
    const override = window.__nyFakeGamepads;
    const src = (override && override()) || (typeof navigator !== "undefined" && typeof navigator.getGamepads === "function" ? navigator.getGamepads() : null);
    const out = [];
    if (Array.isArray(src)) {
      for (const p of src) if (p && p.connected) out.push(p);
    }
    return out;
  }

  function refreshGamepadStatus() {
    canvas.dataset.gamepadCount = String(readGamepads().length);
  }

  /* Synchronous PCM WAV decoder: parse RIFF/WAVE fmt+data and hand the samples
     to Web Audio as an AudioBuffer. Only the async decodeAudioData handles
     compressed formats (MP3/OGG); WAV PCM is decoded here so the browser asset
     gate can assert decode + playback deterministically. */
  function decodeWavPcm(context, bytes) {
    if (!bytes || bytes.byteLength < 44) return null;
    const u8 = new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const tag = (i, s) => u8[i] === s.charCodeAt(0) && u8[i + 1] === s.charCodeAt(1) && u8[i + 2] === s.charCodeAt(2) && u8[i + 3] === s.charCodeAt(3);
    if (!tag(0, "RIFF") || !tag(8, "WAVE")) return null;
    const findChunk = (id) => {
      let off = 12;
      const view = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
      while (off + 8 <= u8.byteLength) {
        const size = view.getUint32(off + 4, true);
        if (tag(off, id)) return { off, size };
        off += 8 + size + (size & 1);
      }
      return null;
    };
    const fmt = findChunk("fmt ");
    const data = findChunk("data");
    if (!fmt || !data || data.size === 0) return null;
    const view = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
    const audioFormat = view.getUint16(fmt.off + 8, true);
    const channels = view.getUint16(fmt.off + 10, true);
    const sampleRate = view.getUint32(fmt.off + 12, true);
    const bitsPerSample = view.getUint16(fmt.off + 22, true);
    if (channels < 1 || sampleRate < 1 || bitsPerSample < 8) return null;
    const pcm = audioFormat === 1;
    const bytesPerSample = bitsPerSample / 8;
    const frames = Math.floor(data.size / (bytesPerSample * channels));
    const buffer = context.createBuffer(channels, frames, sampleRate);
    const dataOff = data.off + 8;
    for (let c = 0; c < channels; c++) {
      const ch = buffer.getChannelData(c);
      for (let f = 0; f < frames; f++) {
        const idx = dataOff + (f * channels + c) * bytesPerSample;
        let sample = 0;
        if (pcm) {
          if (bitsPerSample === 8) sample = (u8[idx] - 128) / 128;
          else if (bitsPerSample === 16) sample = view.getInt16(idx, true) / 32768;
          else if (bitsPerSample === 24) {
            const b0 = u8[idx], b1 = u8[idx + 1], b2 = u8[idx + 2];
            let v = (b0 << 8) | (b1 << 16) | (b2 << 24);
            if (v & 0x800000) v |= 0xff000000;
            sample = v / 8388608;
          } else if (bitsPerSample === 32) sample = view.getInt32(idx, true) / 2147483648;
        } else {
          sample = view.getFloat32(idx, true);
        }
        ch[f] = Math.max(-1, Math.min(1, sample));
      }
    }
    return buffer;
  }

  function wantsCliStage(meta = currentMeta, runtime = currentRuntime) {
    if (runtime && runtime.oneShot) return true;
    const mode = String((meta && meta.mode) || "").toLowerCase();
    return mode === "bench" || mode === "cli" || mode === "native" || mode === "os" || mode === "test";
  }

  function setStageMode(kind) {
    document.body.classList.toggle("cli-mode", kind === "cli");
    document.body.classList.toggle("web-mode", kind === "web");
    if (mirrorOutputInput) mirrorOutputInput.disabled = kind === "cli";
  }

  function refreshCliStage() {
    if (!cliStageOutput) return;
    if (!document.body.classList.contains("cli-mode")) return;
    cliStageOutput.textContent = outputLines.join("\n");
    cliStageOutput.scrollTop = cliStageOutput.scrollHeight;
  }

  function esc(value) {
    return String(value ?? "").replace(/[&<>"']/g, (m) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[m]);
  }

  function mirrorInspectorLog() {
    if (!out) return;
    out.textContent = outputLines.join("\n");
    if (!document.body.classList.contains("cli-mode")) out.scrollTop = out.scrollHeight;
  }

  function log(lines) {
    outputLines = Array.isArray(lines) ? lines.map((x) => String(x)) : [String(lines ?? "")];
    mirrorInspectorLog();
    refreshCliStage();
  }

  function appendLog(...lines) {
    for (const line of lines) outputLines.push(String(line ?? ""));
    mirrorInspectorLog();
    refreshCliStage();
  }

  function resetOutput(lines) {
    stdoutLine = "";
    log(lines);
  }

  function appendStdout(text) {
    stdoutLine += String(text ?? "");
  }

  function flushStdout() {
    appendLog(stdoutLine);
    stdoutLine = "";
  }

  function setStatus(id, text, cls = "") {
    const el = $(id);
    if (el) {
      el.textContent = text;
      el.className = "status-pill" + (cls ? " " + cls : "");
    }
  }

  function setKernelStatus(text = "", cls = "") {
    const ready = currentRuntime && currentMeta && currentRuntime.id === currentMeta.id;
    setStatus("wasmStatus", text || (ready ? (currentRuntime.oneShot ? "Native" : "Web Frame") : "None"), cls || (ready ? "ready" : "warn"));
  }

  function audioState() {
    if (audioUnavailable || typeof window.AudioContext !== "function") return "unavailable";
    if (!audioContext) return "ready";
    return audioContext.state || "suspended";
  }

  function refreshAudioStatus() {
    const state = audioState();
    const text = state === "running" ? "Audio" : state === "suspended" ? "Audio suspended" :
      state === "ready" ? "Audio ready" : "Audio unavailable";
    setStatus("audioStatus", text, state === "running" ? "ready" : state === "unavailable" ? "warn" : "");
    canvas.dataset.audioState = state;
  }

  function ensureAudio() {
    if (audioUnavailable || typeof window.AudioContext !== "function") {
      audioUnavailable = true;
      refreshAudioStatus();
      return null;
    }
    if (!audioContext) {
      try {
        audioContext = new window.AudioContext();
        audioContext.addEventListener("statechange", refreshAudioStatus);
      } catch (_) {
        audioUnavailable = true;
      }
    }
    refreshAudioStatus();
    return audioContext;
  }

  function resumeAudio() {
    const context = ensureAudio();
    if (!context || context.state === "running") return;
    context.resume().catch(() => {}).finally(refreshAudioStatus);
  }

  function shutdownAudio() {
    const context = audioContext;
    audioContext = null;
    if (context && context.state !== "closed") context.close().catch(() => {}).finally(refreshAudioStatus);
    else refreshAudioStatus();
  }

  function resetBrowserBoundary() {
    clearInput();
    shutdownAudio();
    if (document.pointerLockElement && document.exitPointerLock) {
      try { document.exitPointerLock(); } catch (_) {}
    }
    input.touches.clear();
    input.touchStarts.clear();
  }

  function fullscreenActive() {
    return document.fullscreenElement === canvas;
  }

  function pointerLockActive() {
    return document.pointerLockElement === canvas;
  }

  function refreshBrowserRequestState() {
    canvas.dataset.fullscreen = fullscreenActive() ? "1" : "0";
    canvas.dataset.pointerLock = pointerLockActive() ? "1" : "0";
  }

  function requestFullscreen(enabled) {
    if (enabled) {
      if (!fullscreenActive() && typeof canvas.requestFullscreen === "function") {
        canvas.requestFullscreen().catch(() => {}).finally(refreshBrowserRequestState);
      }
    } else if (document.fullscreenElement && typeof document.exitFullscreen === "function") {
      document.exitFullscreen().catch(() => {}).finally(refreshBrowserRequestState);
    }
    refreshBrowserRequestState();
    return fullscreenActive();
  }

  function requestPointerLock(enabled) {
    if (enabled) {
      if (!pointerLockActive() && typeof canvas.requestPointerLock === "function") {
        try {
          const pending = canvas.requestPointerLock();
          if (pending && typeof pending.catch === "function") pending.catch(() => {}).finally(refreshBrowserRequestState);
        } catch (_) {}
      }
    } else if (pointerLockActive() && typeof document.exitPointerLock === "function") {
      document.exitPointerLock();
    }
    refreshBrowserRequestState();
    return pointerLockActive();
  }

  function runtimeModeText(runtime) {
    if (!runtime) return "initializing...";
    if (runtime.oneShot) return "native wasm";
    return "browser runnable";
  }

  function runtimeHeader(meta, runtime) {
    const entry = runtime && runtime.entry ? runtime.entry : "none";
    return [
      meta.source || meta.title || meta.id,
      meta.wasm ? `wasm=${meta.wasm}` : "wasm=local file",
      `entry=${entry}`,
      `argv=${refreshRunArgv(meta).join(" ")}`,
      runtimeModeText(runtime),
    ];
  }

  function setStageSize(w = canvas.width || 1280, h = canvas.height || 720) {
    if (stage.width !== w) stage.width = w;
    if (stage.height !== h) stage.height = h;
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.imageSmoothingEnabled = false;
    ctx.fillStyle = "#050708";
    ctx.fillRect(0, 0, w, h);
  }

  function drawStatusSurface(title, lines = []) {
    frameTouched = true;
    setStageSize();
    ctx.fillStyle = "#050608";
    ctx.fillRect(0, 0, stage.width, stage.height);
    ctx.fillStyle = "#0b0d10";
    ctx.fillRect(96, 76, 1088, 568);
    ctx.strokeStyle = "#1d1f24";
    ctx.strokeRect(96.5, 76.5, 1087, 567);
    ctx.fillStyle = "#edf2ef";
    ctx.font = "30px 'Nytrix Monocraft', ui-monospace, monospace";
    ctx.fillText(title, 132, 128);
    // branding
    ctx.font = "12px 'Nytrix Monocraft', ui-monospace, monospace";
    ctx.textAlign = "right";
    ctx.fillStyle = "rgba(147, 160, 155, 0.35)";
    ctx.fillText("NYTRIX", 1184, 114);
    ctx.textAlign = "left";
    // lines
    ctx.font = "15px 'Nytrix Monocraft', ui-monospace, monospace";
    ctx.fillStyle = "#8a9691";
    lines.forEach((line, i) => ctx.fillText(String(line), 132, 180 + i * 26));
    present();
  }

  function wrapCanvasLine(text, maxChars) {
    const words = String(text ?? "").split(/(\s+)/);
    const lines = [];
    let line = "";
    for (const word of words) {
      if (!word) continue;
      if ((line + word).length > maxChars && line.trim().length) {
        lines.push(line.trimEnd());
        line = word.trimStart();
      } else {
        line += word;
      }
    }
    if (line.length) lines.push(line.trimEnd());
    return lines.length ? lines : [""];
  }

  function drawOutputSurface(title = "Output") {
    if (wantsCliStage()) {
      setStageMode("cli");
      refreshCliStage();
      return;
    }
    if (mirrorOutputInput && !mirrorOutputInput.checked) return;
    frameTouched = true;
    setStageSize();
    ctx.fillStyle = "#08090a";
    ctx.fillRect(96, 76, 1088, 568);
    ctx.strokeStyle = "#1d1f24";
    ctx.strokeRect(96.5, 76.5, 1087, 567);
    ctx.fillStyle = "#edf2ef";
    ctx.font = "25px 'Nytrix Monocraft', ui-monospace, monospace";
    ctx.fillText(title, 132, 126);
    // branding
    ctx.font = "12px 'Nytrix Monocraft', ui-monospace, monospace";
    ctx.textAlign = "right";
    ctx.fillStyle = "rgba(147, 160, 155, 0.35)";
    ctx.fillText("NYTRIX", 1184, 114);
    ctx.textAlign = "left";
    ctx.font = "13px 'Nytrix Monocraft', ui-monospace, monospace";
    ctx.fillStyle = "#93a09b";
    const visible = [];
    for (const line of outputLines) {
      for (const wrapped of wrapCanvasLine(line, 112)) visible.push(wrapped);
    }
    const start = Math.max(0, visible.length - 26);
    visible.slice(start).forEach((line, i) => ctx.fillText(line, 132, 160 + i * 18));
    present();
  }

  function drawEmptySurface() {
    drawStatusSurface("Load a Ny module", [
      "Expected exports: ny_web_frame, ny_web_render, ny_web_main, or main.",
      "Optional calls: ny_web_clear, ny_web_rect, ny_web_line, ny_web_text, ny_web_present."
    ]);
  }

  async function preloadAssets(meta) {
    assetFonts = new Map();
    loadedAssetCount = 0;
    const assets = Array.isArray(meta.assets) ? meta.assets : [];
    const sha256 = async (bytes) => {
      if (!window.crypto || !window.crypto.subtle) return "";
      const digest = await window.crypto.subtle.digest("SHA-256", bytes);
      return Array.from(new Uint8Array(digest), b => b.toString(16).padStart(2, "0")).join("");
    };
    const pack = meta.assetPack && typeof meta.assetPack === "object" ? meta.assetPack : null;
    let packedBytes = null;
    if (pack && typeof pack.url === "string") {
      const response = await fetch(pack.url);
      if (!response.ok) throw new Error(`${meta.id}: asset pack fetch failed (${response.status})`);
      packedBytes = new Uint8Array(await response.arrayBuffer());
      if (Number.isFinite(pack.bytes) && packedBytes.byteLength !== Number(pack.bytes)) {
        throw new Error(`${meta.id}: asset pack size mismatch`);
      }
    }
    for (const item of assets) {
      const path = typeof item === "string" ? item : String(item && item.path || "");
      if (!path) continue;
      let bytes;
      if (packedBytes && item && typeof item === "object" && Number.isInteger(item.offset) && Number.isInteger(item.size)) {
        const offset = Number(item.offset);
        const size = Number(item.size);
        if (offset < 0 || size < 0 || offset + size > packedBytes.byteLength) {
          throw new Error(`${meta.id}: invalid packed asset range for ${path}`);
        }
        bytes = packedBytes.slice(offset, offset + size);
      } else {
        const url = typeof item === "string" ? item : String(item && item.url || path);
        const response = await fetch(url);
        if (!response.ok) throw new Error(`${meta.id}: asset fetch failed (${response.status}) ${path}`);
        bytes = new Uint8Array(await response.arrayBuffer());
      }
      if (item && typeof item === "object" && typeof item.sha256 === "string") {
        const digest = await sha256(bytes);
        if (digest && digest !== item.sha256) throw new Error(`${meta.id}: asset hash mismatch for ${path}`);
      }
      loadedAssetCount++;
      assetCache.set(path, bytes);
      const base = path.split("/").pop();
      if (base && base !== path) assetCache.set(base, bytes);
      if (item && item.preload !== false && /\.(ttf|otf|woff2?)$/i.test(path) && typeof FontFace !== "undefined") {
        const family = `ny-${nextFontId++}`;
        const face = new FontFace(family, bytes.buffer);
        await face.load();
        document.fonts.add(face);
        assetFonts.set(path, { family, size: 0 });
        if (base && base !== path) assetFonts.set(base, { family, size: 0 });
      }
    }
    canvas.dataset.assetsLoaded = String(loadedAssetCount);
  }

  function splitArgs(text) {
    const src = String(text || "");
    const out = [];
    let cur = "";
    let quote = "";
    let escNext = false;
    for (let i = 0; i < src.length; i++) {
      const ch = src[i];
      if (escNext) { cur += ch; escNext = false; continue; }
      if (ch === "\\") { escNext = true; continue; }
      if (quote) { if (ch === quote) quote = ""; else cur += ch; continue; }
      if (ch === "'" || ch === "\"") { quote = ch; continue; }
      if (/\s/.test(ch)) { if (cur.length) { out.push(cur); cur = ""; } continue; }
      cur += ch;
    }
    if (escNext) cur += "\\";
    if (cur.length) out.push(cur);
    return out;
  }

  function refreshRunArgv(meta = currentMeta) {
    const userArgs = splitArgs(runArgsInput ? runArgsInput.value : "");
    const id = meta && meta.id ? String(meta.id) : "module";
    runArgv = ["ny", "--wasm", id, ...userArgs];
    return runArgv;
  }

  function selectedEntry(exports) {
    const requested = runEntryInput ? runEntryInput.value.trim() : "";
    if (requested && typeof exports[requested] === "function") return requested;
    if (requested && exports) appendLog(`entry not found: ${requested}`);
    return ENTRY_ORDER.find((name) => typeof exports[name] === "function") || "";
  }

  function fitCanvas() {
    const rect = canvas.getBoundingClientRect();
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const w = Math.max(1, Math.floor(rect.width * dpr));
    const h = Math.max(1, Math.floor(rect.height * dpr));
    if (canvas.width !== w || canvas.height !== h) {
      canvas.width = w;
      canvas.height = h;
      if (gl) gl.viewport(0, 0, w, h);
    }
    if (stage.width !== w) stage.width = w;
    if (stage.height !== h) stage.height = h;
    canvas.dataset.canvasSize = `${w}x${h}`;
    canvas.dataset.framebuffer = `${stage.width}x${stage.height}`;
  }

  function clearInput() {
    input.codes.clear();
    input.pressed.clear();
    input.buttons.clear();
    input.pressedButtons.clear();
    input.touchStarts.clear();
  }

  function shader(type, source) {
    const s = gl.createShader(type);
    gl.shaderSource(s, source);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
    return s;
  }

  function initGL() {
    gl = canvas.getContext("webgl2", { alpha: false, antialias: false, preserveDrawingBuffer: true });
    if (!gl) { setStatus("webglStatus", "WebGL2 missing", "warn"); return false; }
    const vs = shader(gl.VERTEX_SHADER, "attribute vec2 p;varying vec2 v;void main(){v=(p+1.0)*0.5;gl_Position=vec4(p,0.0,1.0);}");
    const fs = shader(gl.FRAGMENT_SHADER, "precision mediump float;varying vec2 v;uniform sampler2D tex;void main(){gl_FragColor=texture2D(tex,vec2(v.x,1.0-v.y));}");
    program = gl.createProgram();
    gl.attachShader(program, vs);
    gl.attachShader(program, fs);
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(program));
    gl.useProgram(program);
    gl.disable(gl.DEPTH_TEST);
    gl.disable(gl.CULL_FACE);
    gl.disable(gl.BLEND);
    const buf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1]), gl.STATIC_DRAW);
    const loc = gl.getAttribLocation(program, "p");
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
    tex = gl.createTexture();
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, tex);
    /* The stage contains UI, pixel art, and bitmap fonts. Filtering this
       final blit changes authored pixels, so scale it exactly. */
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    const texLoc = gl.getUniformLocation(program, "tex");
    if (texLoc) gl.uniform1i(texLoc, 0);
    webgl3d = makeWebgl3dRenderer();
    setStatus("webglStatus", "WebGL2", "ready");
    canvas.dataset.presentFilter = "nearest";
    fitCanvas();
    return true;
  }

  function makeWebgl3dRenderer() {
    if (!gl) return null;
    const vs = shader(gl.VERTEX_SHADER, `#version 300 es
      in vec3 p; uniform vec3 origin; uniform vec3 camera; uniform float scale; uniform float aspect; uniform float angle;
      void main(){ float c=cos(angle), s=sin(angle); vec3 q=vec3(c*p.x+s*p.z,p.y,-s*p.x+c*p.z)*scale+origin;
        q-=camera; gl_Position=vec4((q.x/-q.z)*1.8/aspect,(q.y/-q.z)*1.8,(-q.z-0.1)/12.0,1.0); }`);
    const fs = shader(gl.FRAGMENT_SHADER, `#version 300 es
      precision mediump float; uniform vec4 color; out vec4 outColor; void main(){ outColor=color; }`);
    const prog = gl.createProgram();
    gl.attachShader(prog, vs); gl.attachShader(prog, fs); gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(prog));
    const points = new Float32Array([
      -1,-1, 1, 1,-1, 1, 1, 1, 1, -1,-1, 1, 1, 1, 1, -1, 1, 1,
       1,-1,-1,-1,-1,-1,-1, 1,-1, 1,-1,-1,-1, 1,-1, 1, 1,-1,
      -1,-1,-1,-1,-1, 1,-1, 1, 1,-1,-1,-1,-1, 1, 1,-1, 1,-1,
       1,-1, 1, 1,-1,-1, 1, 1,-1, 1,-1, 1, 1, 1,-1, 1, 1, 1,
      -1, 1, 1, 1, 1, 1, 1, 1,-1,-1, 1, 1, 1, 1,-1,-1, 1,-1,
      -1,-1,-1, 1,-1,-1, 1,-1, 1,-1,-1,-1, 1,-1, 1,-1,-1, 1,
    ]);
    const buffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer); gl.bufferData(gl.ARRAY_BUFFER, points, gl.STATIC_DRAW);
    return { prog, buffer, pos: gl.getAttribLocation(prog, "p"), origin: gl.getUniformLocation(prog, "origin"), camera: gl.getUniformLocation(prog, "camera"),
      scale: gl.getUniformLocation(prog, "scale"), aspect: gl.getUniformLocation(prog, "aspect"),
      angle: gl.getUniformLocation(prog, "angle"), color: gl.getUniformLocation(prog, "color"),
      cameraPosition: [0, 0, 4], active: false, drawn: false };
  }

  function beginWebgl3d() {
    if (!gl || !webgl3d) return false;
    webgl3d.active = true;
    webgl3d.drawn = true;
    ctx.clearRect(0, 0, stage.width, stage.height);
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LEQUAL);
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    return true;
  }

  function drawCube(memoryRef, position, size, color) {
    if (!gl || !webgl3d || !webgl3d.active) return false;
    const at = (i) => ny.numeric(memoryRef, ny.listGet(memoryRef, position, ny.tag(i), ny.tag(0)));
    const c = Number(color || 0) >>> 0;
    const colorLength = ny.listLen(memoryRef, color);
    const channels = colorLength >= 3 ? [0, 1, 2].map((i) => {
      const value = ny.listGet(memoryRef, color, ny.tag(i), 0n);
      return Math.max(0, Math.min(1, ny.numeric(memoryRef, value)));
    }).concat(colorLength >= 4 ? Math.max(0, Math.min(1, ny.numeric(memoryRef, ny.listGet(memoryRef, color, ny.tag(3), 0n)))) : 1)
      : [(c & 255) / 255, ((c >>> 8) & 255) / 255, ((c >>> 16) & 255) / 255, ((c >>> 24) & 255) / 255 || 1];
    gl.useProgram(webgl3d.prog); gl.bindBuffer(gl.ARRAY_BUFFER, webgl3d.buffer);
    gl.enableVertexAttribArray(webgl3d.pos); gl.vertexAttribPointer(webgl3d.pos, 3, gl.FLOAT, false, 0, 0);
    gl.uniform3f(webgl3d.origin, at(0), at(1), at(2)); gl.uniform1f(webgl3d.scale, Math.max(0.001, Number(size) || 1));
    gl.uniform3fv(webgl3d.camera, webgl3d.cameraPosition);
    gl.uniform1f(webgl3d.aspect, Math.max(1, canvas.width) / Math.max(1, canvas.height));
    gl.uniform1f(webgl3d.angle, performance.now() * 0.0007);
    gl.uniform4f(webgl3d.color, channels[0], channels[1], channels[2], channels[3]);
    const translucent = channels[3] < 0.999;
    if (translucent) {
      gl.enable(gl.BLEND);
      gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
      gl.depthMask(false);
    }
    gl.drawArrays(gl.TRIANGLES, 0, 36);
    if (translucent) {
      gl.depthMask(true);
      gl.disable(gl.BLEND);
      canvas.dataset.webgl3dAlpha = "1";
    }
    canvas.dataset.framePixels = "1";
    canvas.dataset.webgl3d = "1";
    return true;
  }

  function present() {
    setStageMode("web");
    if (!ctx) return;
    if (!gl || gl.isContextLost()) {
      canvas.style.backgroundImage = `url("${stage.toDataURL("image/png")}")`;
      canvas.style.backgroundSize = "100% 100%";
      canvas.style.backgroundRepeat = "no-repeat";
      return;
    }
    const overlay3d = webgl3d && webgl3d.drawn;
    if (overlay3d) webgl3d.drawn = false;
    canvas.style.backgroundImage = "none";
    fitCanvas();
    try {
      gl.useProgram(program);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, tex);
      const frame = ctx.getImageData(0, 0, stage.width, stage.height);
      if (!canvas.dataset.framePixels) {
        for (let i = 0; i < frame.data.length; i += 4) {
          if (frame.data[i] || frame.data[i + 1] || frame.data[i + 2]) {
            canvas.dataset.framePixels = "1";
            break;
          }
        }
      }
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, stage.width, stage.height, 0, gl.RGBA, gl.UNSIGNED_BYTE, frame.data);
      if (overlay3d) {
        gl.disable(gl.DEPTH_TEST);
        gl.enable(gl.BLEND);
        gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
      } else {
        gl.disable(gl.BLEND);
        gl.clearColor(0, 0, 0, 1);
        gl.clear(gl.COLOR_BUFFER_BIT);
      }
      gl.drawArrays(gl.TRIANGLES, 0, 6);
      if (overlay3d) gl.disable(gl.BLEND);
      presentCount++;
      canvas.dataset.presented = String(presentCount);
    } catch (_) { setStatus("webglStatus", "WebGL lost", "warn"); }
  }

  // Mirror of rt_runtime_tag_raw_name (src/rt/shared.h) — maps Ny type name strings to
  // their integer tag values used by the runtime. Must stay in sync with shared.h constants.
  function nyRuntimeTagRaw(name) {
    switch (name) {
      case "nil":      return 0;
      case "int":      return 1;
      case "ffi_ptr":  return 6;
      case "list":     return 100;
      case "dict":     return 101;
      case "dict_tbl": return 108;
      case "set":      return 102;
      case "tuple":    return 103;
      case "ok":       return 104;
      case "err":      return 105;
      case "range":    return 106;
      case "closure":  return 107;
      case "ptr":      return 107; // TAG_CLOSURE matches ptr per shared.h
      case "float":    return 110;
      case "complex":  return 111;
      case "str":      return 120;
      case "str_const":return 121;
      case "bytes":    return 122;
      case "bigint":   return 130;
      case "bigfloat": return 131;
      case "kwarg":    return 150;
      default:         return 0;
    }
  }

  const ny = {

    ptr(v) { return typeof v === "bigint" ? Number(v) : Number(v || 0); },
    int(v) {
      if (typeof v !== "bigint") return Number(v || 0);
      const x = BigInt.asIntN(64, v);
      return Number((x & 1n) === 1n ? (x >> 1n) : x);
    },
    tag(v) { return (BigInt(Math.trunc(Number(v) || 0)) << 1n) | 1n; },
    bool(v) { return v ? NY_TRUE : NY_FALSE; },
    u8(memoryRef) { return new Uint8Array((memoryRef.memory || fallbackMemory).buffer); },
    tagof(memoryRef, value) {
      const v = BigInt.asIntN(64, BigInt(value || 0));
      if ((v & 1n) === 1n) return 0n;
      const p = Number(v);
      if (p <= 8 || p > (memoryRef.memory || fallbackMemory).buffer.byteLength) return 0n;
      return new DataView(ny.u8(memoryRef).buffer).getBigInt64(p - 8, true);
    },
    isFloat(memoryRef, value) {
      const v = BigInt.asIntN(64, BigInt(value || 0));
      return (v & 1n) === 1n || ny.tagof(memoryRef, v) === 110n;
    },
    text(memoryRef, ptr, len) {
      const p = ny.ptr(ptr);
      let n = Number(len || 0);
      if (typeof len === "bigint" && (len & 1n) === 1n) n = Number(len >> 1n);
      if (n <= 0 && p > 16) {
        try {
          const taggedLen = new DataView(ny.u8(memoryRef).buffer).getBigInt64(p - 16, true);
          n = (taggedLen & 1n) === 1n ? Number(taggedLen >> 1n) : Number(taggedLen);
        } catch (_) { n = 0; }
      }
      if (p <= 0 || n <= 0) return "";
      return dec.decode(ny.u8(memoryRef).subarray(p, p + n));
    },
    valueToString(memoryRef, value) {
      const v = BigInt.asIntN(64, BigInt(value || 0));
      if (v === 0n) return "nil";
      if (v === NY_TRUE) return "true";
      if (v === NY_FALSE) return "false";
      const tag = ny.tagof(memoryRef, v);
      if (tag === 120n || tag === 121n) return ny.text(memoryRef, v, 0);
      if (tag === 100n) return `[list ${ny.listLen(memoryRef, v)}]`;
      if (ny.isFloat(memoryRef, v)) return String(ny.flt(memoryRef, v));
      if ((v & 1n) === 1n) return String(Number(v >> 1n));
      return String(Number(v));
    },
    equal(memoryRef, a, b) {
      const left = BigInt.asIntN(64, BigInt(a || 0));
      const right = BigInt.asIntN(64, BigInt(b || 0));
      if (left === right) return true;
      if (ny.isFloat(memoryRef, left) || ny.isFloat(memoryRef, right))
        return ny.numeric(memoryRef, left) === ny.numeric(memoryRef, right);
      const leftTag = ny.tagof(memoryRef, left);
      const rightTag = ny.tagof(memoryRef, right);
      if ((leftTag === 120n || leftTag === 121n) &&
          (rightTag === 120n || rightTag === 121n))
        return ny.text(memoryRef, left, 0) === ny.text(memoryRef, right, 0);
      return false;
    },
    alloc(memoryRef, bytes) {
      const mem = memoryRef.memory || fallbackMemory;
      const n = Math.max(1, Number(bytes || 0));
      let p = memoryRef.heapTop || 1048576;
      p = (p + 15) & ~15;
      const next = p + n;
      if (next >= mem.buffer.byteLength) {
        const need = Math.ceil((next - mem.buffer.byteLength) / 65536);
        try { mem.grow(Math.max(1, need)); } catch (_) { return 0; }
      }
      memoryRef.heapTop = next;
      return p;
    },
    writeI64(memoryRef, off, value) {
      new DataView(ny.u8(memoryRef).buffer).setBigInt64(Number(off), BigInt.asIntN(64, BigInt(value || 0)), true);
    },
    readI64(memoryRef, off) {
      const p = Number(off);
      if (p < 0 || p + 8 > (memoryRef.memory || fallbackMemory).buffer.byteLength) return 0n;
      return new DataView(ny.u8(memoryRef).buffer).getBigInt64(p, true);
    },
    string(memoryRef, text) {
      const bytes = enc.encode(String(text ?? ""));
      const p = ny.alloc(memoryRef, bytes.length + 17) + 16;
      const mem = ny.u8(memoryRef);
      const view = new DataView(mem.buffer);
      view.setBigInt64(p - 16, BigInt((bytes.length << 1) | 1), true);
      view.setBigInt64(p - 8, 120n, true);
      mem.set(bytes, p);
      mem[p + bytes.length] = 0;
      return BigInt(p);
    },
    list(memoryRef, cap) {
      const n = Math.max(0, Math.trunc(Number(cap) || 0));
      const p = ny.alloc(memoryRef, 16 + n * 8 + 16) + 16;
      ny.writeI64(memoryRef, p - 8, 100n);
      ny.writeI64(memoryRef, p, 1n); // Length = 0 (tagged: 1n)
      ny.writeI64(memoryRef, p + 8, ny.tag(n)); // Capacity = n (tagged)
      return BigInt(p);
    },
    listLen(memoryRef, list) {
      const p = ny.ptr(list);
      if (p <= 0) return 0;
      const v = ny.readI64(memoryRef, p);
      return (v & 1n) === 1n ? Number(v >> 1n) : Number(v);
    },
    listCap(memoryRef, list) {
      const p = ny.ptr(list);
      if (p <= 0) return 0;
      const v = ny.readI64(memoryRef, p + 8);
      return (v & 1n) === 1n ? Number(v >> 1n) : Number(v);
    },
    listSetLen(memoryRef, list, len) {
      // Always normalise: untag if tagged, then retag. This is idempotent for
      // tagged BigInt values coming from wasm imports AND correct for raw numbers
      // coming from internal callers like listAppend, rt_range_new, etc.
      const p = ny.ptr(list);
      if (p <= 0) return 0n;
      const tagged = ny.tag(ny.int(len));
      ny.writeI64(memoryRef, p, tagged);
      return tagged;
    },
    listGet(memoryRef, list, idx, fallback = 0n) {
      const p = ny.ptr(list);
      const i = ny.int(idx);
      if (p <= 0 || i < 0 || i >= ny.listLen(memoryRef, list)) return fallback || 0n;
      return ny.readI64(memoryRef, p + 16 + i * 8);
    },
    listSet(memoryRef, list, idx, value) {
      const p = ny.ptr(list);
      const i = ny.int(idx);
      if (p > 0 && i >= 0) ny.writeI64(memoryRef, p + 16 + i * 8, value);
      return value || 0n;
    },
    listAppend(memoryRef, list, value) {
      let p = ny.ptr(list);
      let len = ny.listLen(memoryRef, list);
      let cap = ny.listCap(memoryRef, list);
      if (p <= 0 || len >= cap) {
        const nextCap = Math.max(cap === 0 ? 8 : cap * 2, len + 1);
        const out = ny.list(memoryRef, nextCap);
        for (let i = 0; i < len; i++) ny.listSet(memoryRef, out, ny.tag(i), ny.readI64(memoryRef, p + 16 + i * 8));
        // Write current len to new list (list() starts with len=0 already, but be explicit)
        ny.writeI64(memoryRef, ny.ptr(out), ny.tag(len));
        list = out;
        p = ny.ptr(list);
        cap = nextCap;
      }
      ny.writeI64(memoryRef, p + 16 + len * 8, value);
      // Advance length by 1 using ny.tag to avoid the tagged/raw ambiguity
      ny.writeI64(memoryRef, p, ny.tag(len + 1));
      return list;
    },
    fltBox(memoryRef, bits) {
      const p = ny.alloc(memoryRef, 16) + 8;
      const view = new DataView(ny.u8(memoryRef).buffer);
      view.setBigInt64(p - 8, 110n, true);
      view.setBigInt64(p, BigInt.asIntN(64, BigInt(bits)), true);
      return BigInt(p);
    },
    fltBits(memoryRef, value) {
      const v = BigInt.asIntN(64, BigInt(value || 0));
      if ((v & 1n) === 1n) {
        const buf = new ArrayBuffer(8);
        const view = new DataView(buf);
        view.setFloat64(0, Number(v >> 1n), true);
        return view.getBigInt64(0, true);
      }
      const p = Number(v);
      if (p > 0 && p + 8 <= (memoryRef.memory || fallbackMemory).buffer.byteLength) {
        return new DataView(ny.u8(memoryRef).buffer).getBigInt64(p, true);
      }
      return 0n;
    },
    flt(memoryRef, value) {
      const bits = ny.fltBits(memoryRef, value);
      const buf = new ArrayBuffer(8);
      const view = new DataView(buf);
      view.setBigInt64(0, bits, true);
      return view.getFloat64(0, true);
    },
    fltFromNumber(memoryRef, value) {
      const buf = new ArrayBuffer(8);
      const view = new DataView(buf);
      view.setFloat64(0, Number(value) || 0, true);
      return ny.fltBox(memoryRef, view.getBigInt64(0, true));
    },
    numeric(memoryRef, value) {
      return ny.isFloat(memoryRef, value) ? ny.flt(memoryRef, value) : ny.int(value);
    },
    numericResult(memoryRef, value, preferFloat) {
      return preferFloat ? ny.fltFromNumber(memoryRef, value) : ny.tag(value);
    },
  };

  function rgba(color) {
    const c = Number(color || 0) >>> 0;
    const a = ((c >>> 24) & 255) / 255 || 1;
    return `rgba(${c & 255},${(c >>> 8) & 255},${(c >>> 16) & 255},${a})`;
  }

  function makeWebImports(meta, memoryRef) {
    return {
      ny_web_canvas_width: () => ny.tag(stage.width),
      ny_web_canvas_height: () => ny.tag(stage.height),
      ny_web_time: () => performance.now() / 1000,
      ny_web_key_down: (code) => ny.tag(input.codes.has(Number(code)) ? 1 : 0),
      ny_web_mouse_down: () => ny.tag(input.buttons.has(0) ? 1 : 0),
      ny_web_mouse_x: () => ny.tag(input.mouse[0]),
      ny_web_mouse_y: () => ny.tag(input.mouse[1]),
      ny_web_clear: (r, g, b, a) => {
        frameTouched = true;
        setStageSize();
        ctx.fillStyle = `rgba(${Math.round(Number(r) * 255)},${Math.round(Number(g) * 255)},${Math.round(Number(b) * 255)},${Number(a)})`;
        ctx.fillRect(0, 0, stage.width, stage.height);
        return 0n;
      },
      ny_web_rect: (x, y, w, h, color) => {
        frameTouched = true;
        ctx.fillStyle = rgba(color);
        ctx.fillRect(Number(x), Number(y), Number(w), Number(h));
        return 0n;
      },
      ny_web_rect_f: (x, y, w, h, r, g, b, a) => {
        frameTouched = true;
        ctx.fillStyle = `rgba(${Math.round(Number(r) * 255)},${Math.round(Number(g) * 255)},${Math.round(Number(b) * 255)},${Number(a)})`;
        ctx.fillRect(Number(x), Number(y), Number(w), Number(h));
        return 0n;
      },
      ny_web_line: (x0, y0, x1, y1, color, width = 1) => {
        frameTouched = true;
        ctx.strokeStyle = rgba(color);
        ctx.lineWidth = Math.max(1, Number(width) || 1);
        ctx.beginPath();
        ctx.moveTo(Number(x0), Number(y0));
        ctx.lineTo(Number(x1), Number(y1));
        ctx.stroke();
        return 0n;
      },
      ny_web_text: (ptr, len, x, y, size, color) => {
        frameTouched = true;
        ctx.fillStyle = rgba(color);
        ctx.font = `${Math.max(8, Math.round(Number(size) || 14))}px 'Nytrix Monocraft', ui-monospace, monospace`;
        ctx.textBaseline = "top";
        ctx.fillText(ny.text(memoryRef, ptr, len), Number(x), Number(y));
        return 0n;
      },
      ny_web_log: (ptr, len) => resetOutput([meta.source, ny.text(memoryRef, ptr, len)]),
      ny_web_present: () => { framePresented = true; present(); return 0n; },
    };
  }

  function makeUiImports(memoryRef, asyncifyRef) {
    const bool = (value) => ny.bool(Boolean(value));
    const color = (value, fallback) => {
      if (ny.listLen(memoryRef, value) >= 3) {
        const channel = (index, fallback) => {
          const item = ny.listGet(memoryRef, value, ny.tag(index), 0n);
          return Math.max(0, Math.min(1, item === 0n ? fallback : ny.numeric(memoryRef, item)));
        };
        return `rgba(${Math.round(channel(0, 0) * 255)},${Math.round(channel(1, 0) * 255)},${Math.round(channel(2, 0) * 255)},${channel(3, 1)})`;
      }
      return value === 1n ? fallback : rgba(ny.int(value));
    };
    const list2f = (x, y) => {
      const out = ny.list(memoryRef, 2);
      ny.listSetLen(memoryRef, out, 2);
      ny.listSet(memoryRef, out, ny.tag(0), ny.fltFromNumber(memoryRef, x));
      ny.listSet(memoryRef, out, ny.tag(1), ny.fltFromNumber(memoryRef, y));
      return BigInt(out);
    };
    const list2i = (x, y) => {
      const out = ny.list(memoryRef, 2);
      ny.listSetLen(memoryRef, out, 2);
      ny.listSet(memoryRef, out, ny.tag(0), ny.tag(x));
      ny.listSet(memoryRef, out, ny.tag(1), ny.tag(y));
      return BigInt(out);
    };
    const list4f = (v0, v1, v2, v3) => {
      const out = ny.list(memoryRef, 4);
      ny.listSetLen(memoryRef, out, 4);
      ny.listSet(memoryRef, out, ny.tag(0), ny.fltFromNumber(memoryRef, v0));
      ny.listSet(memoryRef, out, ny.tag(1), ny.fltFromNumber(memoryRef, v1));
      ny.listSet(memoryRef, out, ny.tag(2), ny.fltFromNumber(memoryRef, v2));
      ny.listSet(memoryRef, out, ny.tag(3), ny.fltFromNumber(memoryRef, v3));
      return BigInt(out);
    };
    return {
      "std.os.ui.render.init_window": () => ny.tag(1),
      "std.os.ui.render.close_window": () => { asyncifyRef.closed = true; return NY_TRUE; },
      "std.os.ui.window.set_window_fullscreen": (_win, enabled) => bool(requestFullscreen(ny.int(enabled) !== 0)),
      "std.os.ui.window.is_window_fullscreen": () => bool(fullscreenActive()),
      "std.os.ui.window.set_input_exclusive": (_win, enabled) => bool(requestPointerLock(ny.int(enabled) !== 0)),
      "std.os.ui.render.font_load_first": (paths, size = 0n) => {
        const count = ny.listLen(memoryRef, paths);
        for (let i = 0; i < count; i++) {
          const path = ny.text(memoryRef, ny.listGet(memoryRef, paths, ny.tag(i), 0n), 0);
          const font = assetFonts.get(path);
          if (!font) continue;
          const id = nextFontId++;
          assetFonts.set(id, { family: font.family, size: Math.max(1, ny.int(size)) });
          return ny.tag(id);
        }
        return ny.tag(0);
      },
      "std.os.ui.render.window_should_close": () => bool(asyncifyRef.closed),
      "std.os.ui.window.set_should_close": () => { asyncifyRef.closed = true; return 0n; },
      "std.os.ui.window.close": () => { asyncifyRef.closed = true; return NY_TRUE; },
      "std.os.ui.window.input.key_down": (key) => bool(input.codes.has(ny.int(key))),
      "std.os.ui.window.input.key_pressed": (key) => {
        const code = ny.int(key);
        const pressed = input.pressed.has(code);
        if (pressed) input.pressed.delete(code);
        return bool(pressed);
      },
      "std.os.ui.window.input.mouse_pos": () => list2f(input.mouse[0], input.mouse[1]),
      "std.os.ui.window.input.mouse_button_down": (button = 0n) => bool(input.buttons.has(ny.int(button))),
      "std.os.ui.window.input.mouse_button_pressed": (button = 0n) => {
        const code = ny.int(button);
        const pressed = input.pressedButtons.has(code);
        if (pressed) input.pressedButtons.delete(code);
        return bool(pressed);
      },
      "std.os.ui.window.scroll_pos": () => list2f(input.scroll[0], input.scroll[1]),
      "std.os.ui.window.input.touch_count": () => ny.tag(input.touches.size),
      "std.os.ui.window.input.touch_pos": (index = 0n) => {
        const i = ny.int(index);
        if (i < 0 || i >= input.touches.size) return list2f(0, 0);
        const pos = Array.from(input.touches.values())[i];
        return list2f(pos[0], pos[1]);
      },
      "std.os.ui.window.input.touch_active": (index = 0n) => bool(ny.int(index) >= 0 && ny.int(index) < input.touches.size),
      "std.os.ui.window.gamepad_count": () => ny.tag(readGamepads().length),
      "std.os.ui.window.gamepad_connected": (pad = 0n) => bool(ny.int(pad) >= 0 && ny.int(pad) < readGamepads().length),
      "std.os.ui.window.gamepad_name": (pad = 0n) => {
        const p = readGamepads()[ny.int(pad)];
        return p ? ny.string(memoryRef, String(p.id || "")) : ny.string(memoryRef, "");
      },
      "std.os.ui.window.gamepad_guid": (pad = 0n) => {
        const p = readGamepads()[ny.int(pad)];
        return p ? ny.string(memoryRef, String(p.mapping || "standard")) : ny.string(memoryRef, "");
      },
      "std.os.ui.window.gamepad_axis_count": (pad = 0n) => {
        const p = readGamepads()[ny.int(pad)];
        return p ? ny.tag((p.axes || []).length) : ny.tag(0);
      },
      "std.os.ui.window.gamepad_button_count": (pad = 0n) => {
        const p = readGamepads()[ny.int(pad)];
        return p ? ny.tag((p.buttons || []).length) : ny.tag(0);
      },
      "std.os.ui.window.gamepad_axis": (pad, axis) => {
        const p = readGamepads()[ny.int(pad)];
        const a = p ? (p.axes || [])[ny.int(axis)] : 0;
        return ny.fltFromNumber(memoryRef, Math.abs(a) < 1e-6 ? 0 : a);
      },
      "std.os.ui.window.gamepad_button": (pad, button) => {
        const p = readGamepads()[ny.int(pad)];
        const b = p ? (p.buttons || [])[ny.int(button)] : null;
        return bool(b && !!b.pressed);
      },
      "std.os.ui.window.test_report_touch": (count, x, y) => {
        canvas.dataset.touchCount = String(Number(BigInt.asIntN(64, BigInt(count || 0))));
        canvas.dataset.touchX = String(ny.numeric(memoryRef, x));
        canvas.dataset.touchY = String(ny.numeric(memoryRef, y));
        return NY_TRUE;
      },
      "std.os.ui.window.test_report_gamepad": (count, buttonA, leftX) => {
        const pads = readGamepads();
        const padCount = pads.length;
        let mappedButtonA = 0;
        let mappedLeftX = 0;
        if (pads[0]) {
          const pressed = (pads[0].buttons || [])[0];
          mappedButtonA = pressed && !!pressed.pressed ? 1 : 0;
          mappedLeftX = Number((pads[0].axes || [])[0]) || 0;
        }
        canvas.setAttribute("data-gamepad-count", String(padCount));
        canvas.setAttribute("data-gamepad-buttonA", String(mappedButtonA));
        canvas.setAttribute("data-gamepad-leftX", String(mappedLeftX));
        return NY_TRUE;
      },
      "std.os.ui.render.begin_frame_clear": (fill) => {
        frameTouched = true;
        setStageSize();
        ctx.fillStyle = color(fill, "rgba(0,0,0,1)");
        ctx.fillRect(0, 0, stage.width, stage.height);
        return NY_TRUE;
      },
      "std.os.ui.render.framebuffer_size_f64": () => list2f(stage.width, stage.height),
      "std.os.ui.render.get_framebuffer_size": () => list2i(stage.width, stage.height),
      "std.os.ui.render.get_frame_time": () => webFrameDt,
      "std.os.ui.render.set_ortho_2d": () => 0n,
      "std.os.ui.render.matrix.mat4_identity": () => ny.tag(0),
      "std.os.ui.render.camera_init": (position) => {
        if (webgl3d && ny.listLen(memoryRef, position) >= 3) {
          webgl3d.cameraPosition = [0, 1, 2].map((i) => ny.numeric(memoryRef, ny.listGet(memoryRef, position, ny.tag(i), 0n)));
        }
        return ny.tag(1);
      },
      "std.os.ui.render.begin_mode_3d": () => bool(beginWebgl3d()),
      "std.os.ui.render.end_mode_3d": () => { if (webgl3d) webgl3d.active = false; return NY_TRUE; },
      "std.os.ui.render.draw_cube": (position, size, fill) => bool(drawCube(memoryRef, position, size, fill)),
      "std.os.ui.render.draw_rect": (x, y, w, h, fill) => {
        frameTouched = true;
        ctx.fillStyle = color(fill, "rgba(255,255,255,1)");
        ctx.fillRect(x, y, w, h);
        return 0n;
      },
      "std.os.ui.render.draw_circle": (x, y, radius, fill) => {
        frameTouched = true;
        ctx.fillStyle = color(fill, "rgba(255,255,255,1)");
        ctx.beginPath();
        ctx.arc(x, y, radius, 0, Math.PI * 2);
        ctx.fill();
        return 0n;
      },
      "std.os.ui.render.draw_text": (fontId, text, x, y, fill) => {
        frameTouched = true;
        ctx.fillStyle = color(fill, "rgba(255,255,255,1)");
        const font = assetFonts.get(ny.int(fontId));
        const size = font ? Math.max(1, Math.round(font.size)) : 35;
        ctx.font = font ? `${size}px "${font.family}", monospace` : "35px 'Nytrix Monocraft', ui-monospace, monospace";
        ctx.textBaseline = "top";
        ctx.fillText(ny.text(memoryRef, text, 0), Math.round(x), Math.round(y));
        return 0n;
      },
      "std.os.ui.render.measure_text": (fontId, text) => {
        const font = assetFonts.get(ny.int(fontId));
        const size = font ? Math.max(1, Math.round(font.size)) : 35;
        ctx.font = font ? `${size}px "${font.family}", monospace` : "35px 'Nytrix Monocraft', ui-monospace, monospace";
        const metrics = ctx.measureText(ny.text(memoryRef, text, 0));
        const ascent = Number(metrics.actualBoundingBoxAscent || size * 0.8);
        const descent = Number(metrics.actualBoundingBoxDescent || size * 0.2);
        return list2f(Number(metrics.width), ascent + descent);
      },
      "std.os.ui.render.draw_text_centered": (fontId, text, cx, cy, fill) => {
        frameTouched = true;
        ctx.fillStyle = color(fill, "rgba(255,255,255,1)");
        const font = assetFonts.get(ny.int(fontId));
        const size = font ? Math.max(1, Math.round(font.size)) : 35;
        ctx.font = font ? `${size}px "${font.family}", monospace` : "35px 'Nytrix Monocraft', ui-monospace, monospace";
        ctx.textAlign = "center";
        ctx.textBaseline = "middle";
        ctx.fillText(ny.text(memoryRef, text, 0), Math.round(cx), Math.round(cy));
        ctx.textAlign = "start";
        ctx.textBaseline = "top";
        return 0n;
      },
      "std.os.ui.render.texture_create_rgba": (w, h, pixels, format, filter, wrap_s, wrap_t, use_mipmaps) => {
        const width = ny.int(w);
        const height = ny.int(h);
        const total = width * height * 4;
        const isList = pixels !== 0n && ny.listLen(memoryRef, pixels) >= total;
        if (width <= 0 || height <= 0 || (!isList && !pixels)) return ny.tag(-1);
        const byte = (i) => {
          if (isList) {
            const raw = ny.listGet(memoryRef, pixels, ny.tag(i), 0n);
            return typeof raw === "bigint" ? Number(raw) & 255 : (Number(raw) || 0) & 255;
          }
          return ny.u8(memoryRef)[ny.ptr(pixels) + i] || 0;
        };
        const rgba8 = new Uint8ClampedArray(total);
        for (let i = 0; i < total; i++) rgba8[i] = byte(i);
        const cvs = document.createElement("canvas");
        cvs.width = width;
        cvs.height = height;
        const cctx = cvs.getContext("2d");
        cctx.putImageData(new ImageData(new Uint8ClampedArray(rgba8), width, height), 0, 0);
        const id = nextTextureId++;
        g2dTextures.set(id, { canvas: cvs, w: width, h: height, filter });
        return ny.tag(id);
      },
      "std.os.ui.render.texture_size": (tex) => {
        const t = g2dTextures.get(ny.int(tex));
        return list2i(t ? t.w : 0, t ? t.h : 0);
      },
      "std.os.ui.render.texture_destroy": (tex) => {
        g2dTextures.delete(ny.int(tex));
        return NY_TRUE;
      },
      "std.os.ui.render.draw_texture": (texId, x, y, scale, fill) => {
        const t = g2dTextures.get(ny.int(texId));
        if (!t) return 0n;
        frameTouched = true;
        ctx.imageSmoothingEnabled = false;
        ctx.drawImage(t.canvas,
          Math.round(x),
          Math.round(y),
          t.w * Math.max(0.001, scale),
          t.h * Math.max(0.001, scale));
        return 0n;
      },
      "std.os.ui.render.get_pixel": (x, y) => {
        const data = ctx.getImageData(Math.round(Number(x)), Math.round(Number(y)), 1, 1).data;
        return list4f(data[0] / 255, data[1] / 255, data[2] / 255, data[3] / 255);
      },
      "std.os.ui.render.end_frame": () => {
        framePresented = true;
        present();
        if (asyncifyRef.controller) {
          if (asyncifyRef.controller.rewinding()) asyncifyRef.controller.finishRewind();
          else asyncifyRef.controller.yieldFrame();
        }
        return 0n;
      },
    };
  }

  function makeAsyncify(instance, memoryRef, entry) {
    const state = () => Number(instance.exports.asyncify_get_state());
    const memory = memoryRef.memory;
    const reserve = 8 * 1024 * 1024;
    const header = 16;
    const data = memory.buffer.byteLength;
    memory.grow(Math.ceil((reserve + header) / 65536));
    const view = new DataView(memory.buffer);
    view.setUint32(data, data + header, true);
    view.setUint32(data + 4, data + reserve + header, true);
    view.setUint32(data + 8, 1, true);
    let pending = false;
    let resumeAt = 0;
    let resumeValue = 0n;
    let resumeReady = false;
    let resumeWake = null;
    const notifyResume = () => {
      resumeAt = 0;
      resumeReady = true;
      const wake = resumeWake;
      resumeWake = null;
      if (wake) wake(performance.now());
    };
    return {
      yieldFrame() {
        if (state() !== 0 || pending) return;
        pending = true;
        resumeAt = 0;
        instance.exports.asyncify_start_unwind(data);
      },
      sleep(ms) {
        if (state() === 2) {
          instance.exports.asyncify_stop_rewind();
          return;
        }
        if (state() !== 0 || pending) return;
        pending = true;
        resumeAt = performance.now() + Math.max(0, Number(ms) || 0);
        instance.exports.asyncify_start_unwind(data);
      },
      fetch(load) {
        if (state() === 2) {
          instance.exports.asyncify_stop_rewind();
          const value = resumeValue;
          resumeValue = 0n;
          return value;
        }
        if (state() !== 0 || pending) return 0n;
        pending = true;
        resumeAt = Infinity;
        resumeReady = false;
        Promise.resolve().then(load).then((value) => {
          resumeValue = value == null ? 0n : value;
          notifyResume();
        }, () => {
          resumeValue = 0n;
          notifyResume();
        });
        instance.exports.asyncify_start_unwind(data);
        return 0n;
      },
      resume(dt) {
        webFrameDt = Math.max(1 / 240, Math.min(1 / 20, dt || (1 / 60)));
        pending = false;
        resumeAt = 0;
        instance.exports.asyncify_start_rewind(data);
        try {
          entry();
        } finally {
          if (state() === 2) instance.exports.asyncify_stop_rewind();
        }
      },
      needsResume() { return state() === 1; },
      resumeDelay() { return Math.max(0, resumeAt - performance.now()); },
      waitForResume(wake) {
        if (resumeReady) wake(performance.now());
        else resumeWake = wake;
      },
      rewinding() { return state() === 2; },
      finishUnwind() { if (state() === 1) instance.exports.asyncify_stop_unwind(); },
      finishRewind() { if (state() === 2) instance.exports.asyncify_stop_rewind(); },
    };
  }

  function makeRuntimeImports(meta, memoryRef, asyncifyRef) {
    const virtualFiles = new Map();
    const webSockets = new Map();
    let nextWebSocket = 1;
    const persistedPrefix = "nytrix.vfs.";
    const persistedDirPrefix = "nytrix.vfs.dir.";
    let nextVirtualFd = 0x10000;
    const assetPath = (value) => {
      const path = String(value || "").replaceAll("\\", "/");
      return path.startsWith("./") ? path.slice(2) : path;
    };
    const assetBytes = (value) => {
      const path = assetPath(value);
      return assetCache.get(path) || assetCache.get(path.split("/").pop());
    };
    const base64Bytes = (text) => {
      try {
        const raw = atob(text);
        const bytes = new Uint8Array(raw.length);
        for (let i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i);
        return bytes;
      } catch (_) { return null; }
    };
    const storedBytes = (path) => {
      try {
        if (!window.localStorage) return null;
        const value = window.localStorage.getItem(persistedPrefix + path);
        return value == null ? null : base64Bytes(value);
      } catch (_) { return null; }
    };
    const persistBytes = (path, bytes) => {
      try {
        if (!window.localStorage) return false;
        let binary = "";
        for (let i = 0; i < bytes.length; i += 0x8000) {
          binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
        }
        window.localStorage.setItem(persistedPrefix + path, btoa(binary));
        return true;
      } catch (_) { return false; }
    };
    const removePersisted = (path) => {
      try {
        if (!window.localStorage) return false;
        window.localStorage.removeItem(persistedPrefix + path);
        return true;
      } catch (_) { return false; }
    };
    const storedDir = (path) => {
      try {
        return !!window.localStorage && window.localStorage.getItem(persistedDirPrefix + path) === "1";
      } catch (_) { return false; }
    };
    const persistDir = (path) => {
      try {
        if (!window.localStorage) return false;
        window.localStorage.setItem(persistedDirPrefix + path, "1");
        return true;
      } catch (_) { return false; }
    };
    const removePersistedDir = (path) => {
      try {
        if (!window.localStorage) return false;
        window.localStorage.removeItem(persistedDirPrefix + path);
        return true;
      } catch (_) { return false; }
    };
    const virtualBytes = (path) => storedBytes(path) || assetBytes(path);
    const virtualPath = (value) => assetPath(ny.text(memoryRef, value, 0));
    const virtualDirectory = (path) => {
      const prefix = path && path !== "." ? path.replace(/\/$/, "") + "/" : "";
      const names = new Set();
      const add = (candidate) => {
        const p = assetPath(candidate);
        if (!p.startsWith(prefix)) return;
        const rest = p.slice(prefix.length);
        if (!rest) return;
        names.add(rest.split("/", 1)[0]);
      };
      for (const p of assetCache.keys()) add(p);
      for (const p of virtualFiles.values()) add(p.path);
      try {
        if (window.localStorage) {
          for (let i = 0; i < window.localStorage.length; i++) {
            const key = window.localStorage.key(i) || "";
            if (key.startsWith(persistedPrefix) && !key.startsWith(persistedDirPrefix))
              add(key.slice(persistedPrefix.length));
            else if (key.startsWith(persistedDirPrefix))
              add(key.slice(persistedDirPrefix.length));
          }
        }
      } catch (_) {}
      return [...names].sort();
    };
    const virtualDirectoryExists = (path) => {
      const logical = assetPath(path).replace(/^\/+|\/+$/g, "") || ".";
      return logical === "." || storedDir(logical) || virtualDirectory(logical).length > 0;
    };
    const result = (tag, value) => {
      const p = ny.alloc(memoryRef, 32) + 16;
      ny.writeI64(memoryRef, p - 16, 17n);
      ny.writeI64(memoryRef, p - 8, BigInt(tag));
      ny.writeI64(memoryRef, p, value);
      return BigInt(p);
    };
    const virtualRead = (fd, ptr, len, off) => {
      const handle = virtualFiles.get(ny.int(fd));
      if (!handle) return -9n;
      const start = Math.max(0, handle.offset);
      const count = Math.max(0, Math.min(ny.int(len), handle.bytes.length - start));
      const dest = ny.ptr(ptr) + Math.max(0, ny.int(off));
      const mem = ny.u8(memoryRef);
      if (dest < 0 || dest + count > mem.byteLength) return -14n;
      mem.set(handle.bytes.subarray(start, start + count), dest);
      handle.offset = start + count;
      return BigInt(count);
    };
    const virtualWrite = (fd, ptr, len, off) => {
      const handle = virtualFiles.get(ny.int(fd));
      if (!handle || !handle.writable) return ny.tag(-13);
      const start = Math.max(0, handle.offset);
      const count = Math.max(0, ny.int(len));
      const source = ny.ptr(ptr) + Math.max(0, ny.int(off));
      const mem = ny.u8(memoryRef);
      if (source < 0 || source + count > mem.byteLength) return ny.tag(-14);
      if (start + count > handle.bytes.length) {
        const next = new Uint8Array(start + count);
        next.set(handle.bytes);
        handle.bytes = next;
      }
      handle.bytes.set(mem.subarray(source, source + count), start);
      handle.offset = start + count;
      persistBytes(handle.path, handle.bytes);
      return ny.tag(count);
    };
    const webFetch = (url) => {
      if (!asyncifyRef.controller)
        throw new Error("browser fetch requires Asyncify instrumentation");
      const target = ny.text(memoryRef, url, 0);
      return asyncifyRef.controller.fetch(async () => {
        try {
          const response = await globalThis.fetch(target, { credentials: "same-origin" });
          if (!response.ok) return 0n;
          return ny.string(memoryRef, await response.text());
        } catch (_) {
          return 0n;
        }
      });
    };
    const withClipboardGesture = (operation) => {
      if (navigator.userActivation && navigator.userActivation.isActive)
        return operation();
      canvas.dataset.clipboardState = "waiting-for-gesture";
      return new Promise((resolve) => {
        let settled = false;
        let timer = null;
        const finish = (value) => {
          if (settled) return;
          settled = true;
          if (timer !== null) clearTimeout(timer);
          canvas.removeEventListener("click", onClick);
          resolve(value);
        };
        const onClick = () => {
          canvas.dataset.clipboardState = "gesture-received";
          Promise.resolve().then(operation).then(finish, () => finish(null));
        };
        canvas.addEventListener("click", onClick, { once: true });
        timer = setTimeout(() => {
          canvas.dataset.clipboardState = "gesture-timeout";
          finish(null);
        }, 20000);
      });
    };
    const webClipboardWrite = (text) => {
      if (!asyncifyRef.controller || !navigator.clipboard || typeof navigator.clipboard.writeText !== "function")
        return NY_FALSE;
      const value = ny.text(memoryRef, text, 0);
      return asyncifyRef.controller.fetch(async () => {
        try {
          const result = await withClipboardGesture(() => navigator.clipboard.writeText(value).then(() => true));
          canvas.dataset.clipboardWrite = result === true ? "1" : "0";
          return result === true ? NY_TRUE : NY_FALSE;
        } catch (_) {
          canvas.dataset.clipboardWrite = "error";
          return NY_FALSE;
        }
      });
    };
    const webClipboardRead = () => {
      if (!asyncifyRef.controller || !navigator.clipboard || typeof navigator.clipboard.readText !== "function")
        return 0n;
      return asyncifyRef.controller.fetch(async () => {
        try {
          const result = await withClipboardGesture(() => navigator.clipboard.readText());
          canvas.dataset.clipboardRead = typeof result === "string" ? result : "error";
          return typeof result === "string" ? ny.string(memoryRef, result) : 0n;
        } catch (_) {
          canvas.dataset.clipboardRead = "error";
          return 0n;
        }
      });
    };
    const websocketRecord = (handle) => webSockets.get(ny.int(handle));
    const websocketOpen = (url) => {
      try {
        if (typeof globalThis.WebSocket !== "function") return 0n;
        const target = ny.text(memoryRef, url, 0);
        if (!/^wss?:\/\//i.test(target)) return 0n;
        const socket = new globalThis.WebSocket(target);
        const id = nextWebSocket++;
        const record = { socket, state: "CONNECTING", messages: [] };
        webSockets.set(id, record);
        socket.addEventListener("open", () => { record.state = "OPEN"; });
        socket.addEventListener("message", (event) => {
          if (typeof event.data === "string") record.messages.push(event.data);
        });
        socket.addEventListener("error", () => { record.state = "ERROR"; });
        socket.addEventListener("close", () => { record.state = "CLOSED"; });
        return ny.tag(id);
      } catch (_) {
        return 0n;
      }
    };
    const websocketState = (handle) => {
      const record = websocketRecord(handle);
      if (!record) return 0n;
      const states = ["CONNECTING", "OPEN", "CLOSING", "CLOSED"];
      const state = states[record.socket.readyState] || record.state || "ERROR";
      record.state = state;
      return ny.string(memoryRef, state);
    };
    const websocketSend = (handle, text) => {
      const record = websocketRecord(handle);
      if (!record || record.socket.readyState !== 1) return NY_FALSE;
      try {
        record.socket.send(ny.text(memoryRef, text, 0));
        return NY_TRUE;
      } catch (_) {
        return NY_FALSE;
      }
    };
    const websocketReceive = (handle) => {
      const record = websocketRecord(handle);
      if (!record || record.messages.length === 0) return 0n;
      return ny.string(memoryRef, record.messages.shift());
    };
    const websocketClose = (handle) => {
      const record = websocketRecord(handle);
      if (!record) return NY_FALSE;
      try {
        record.state = "CLOSING";
        record.socket.close();
        return NY_TRUE;
      } catch (_) {
        return NY_FALSE;
      }
    };
    return {
      rt_argc: () => ny.tag(refreshRunArgv(meta).length),
      "std.core.primitives.argc": () => ny.tag(refreshRunArgv(meta).length),
      rt_argv: () => 0n,
      rt_runtime_tag: (v) => {
        // rt_runtime_tag receives a tagged Ny string and returns a tagged integer tag.
        // Used by the RT_DEF("__runtime_tag") path when the arg is not a compile-time literal.
        const name = ny.text(memoryRef, v, 0);
        return ny.tag(nyRuntimeTagRaw(name));
      },
      "std.core.primitives.runtime_tag_raw": (v) => {
        // Surface-level runtime_tag_raw — called from module-init for global def constants
        // like _CORE_TAG_LIST = runtime_tag_raw("list"). Receives tagged Ny string, returns
        // tagged integer matching rt_runtime_tag_raw_name in src/rt/shared.h.
        const name = ny.text(memoryRef, v, 0);
        return ny.tag(nyRuntimeTagRaw(name));
      },
      rt_malloc: (size) => BigInt(ny.alloc(memoryRef, ny.int(size))),
      rt_realloc: (_ptr, size) => BigInt(ny.alloc(memoryRef, ny.int(size))),
      rt_free: () => 0n,
      __access: (path) => virtualBytes(virtualPath(path)) || virtualDirectory(virtualPath(path)).length ? ny.tag(0) : ny.tag(-2),
      __open: (path, flags = 0n) => {
        const logical = virtualPath(path);
        const rawFlags = ny.int(flags);
        const writable = (rawFlags & 3) !== 0;
        let bytes = virtualBytes(logical);
        if (!bytes && !(rawFlags & 64)) return ny.tag(-2);
        if (!bytes) bytes = new Uint8Array(0);
        else bytes = new Uint8Array(bytes);
        if ((rawFlags & 512) !== 0 && writable) bytes = new Uint8Array(0);
        const fd = nextVirtualFd++;
        virtualFiles.set(fd, { path: logical, bytes, offset: (rawFlags & 1024) ? bytes.length : 0, writable });
        return ny.tag(fd);
      },
      __read_off: (fd, ptr, len, off = 0n) => virtualRead(fd, ptr, len, off),
      __write_off: (fd, ptr, len, off = 0n) => virtualWrite(fd, ptr, len, off),
      __close: (fd) => virtualFiles.delete(ny.int(fd)) ? ny.tag(0) : ny.tag(-9),
      __unlink: (path) => {
        const logical = virtualPath(path);
        if (storedBytes(logical) == null) return ny.tag(-2);
        removePersisted(logical);
        return ny.tag(0);
      },
      __rename: (oldPath, newPath) => {
        const oldName = virtualPath(oldPath);
        const newName = virtualPath(newPath);
        const bytes = storedBytes(oldName);
        if (!bytes) return ny.tag(-2);
        if (!persistBytes(newName, bytes)) return ny.tag(-5);
        removePersisted(oldName);
        return ny.tag(0);
      },
      __mkdir: (path) => {
        const logical = virtualPath(path).replace(/^\/+|\/+$/g, "");
        if (!logical || virtualDirectoryExists(logical)) return ny.tag(-17);
        const slash = logical.lastIndexOf("/");
        const parent = slash > 0 ? logical.slice(0, slash) : ".";
        if (!virtualDirectoryExists(parent) || !persistDir(logical)) return ny.tag(-2);
        return ny.tag(0);
      },
      __rmdir: (path) => {
        const logical = virtualPath(path).replace(/^\/+|\/+$/g, "");
        if (!logical || !storedDir(logical)) return ny.tag(-2);
        if (virtualDirectory(logical).length) return ny.tag(-39);
        return removePersistedDir(logical) ? ny.tag(0) : ny.tag(-5);
      },
      __is_dir: (path) => ny.tag(virtualDirectoryExists(virtualPath(path)) ? 1 : 0),
      __dir_open: (path) => {
        const names = virtualDirectory(virtualPath(path));
        if (!names.length) return 0n;
        const fd = nextVirtualFd++;
        virtualFiles.set(fd, { path: virtualPath(path), entries: names, index: 0, directory: true, writable: false });
        return ny.tag(fd);
      },
      __dir_read: (fd) => {
        const handle = virtualFiles.get(ny.int(fd));
        if (!handle || !handle.directory || handle.index >= handle.entries.length) return 0n;
        return ny.string(memoryRef, handle.entries[handle.index++]);
      },
      __dir_close: (fd) => virtualFiles.delete(ny.int(fd)) ? ny.tag(0) : ny.tag(-9),
      rt_memset: (dst, val, count) => { ny.u8(memoryRef).fill(Number(val) & 255, ny.ptr(dst), ny.ptr(dst) + ny.ptr(count)); return 0n; },
      rt_memcpy: (dst, src, count) => { ny.u8(memoryRef).copyWithin(ny.ptr(dst), ny.ptr(src), ny.ptr(src) + ny.ptr(count)); return 0n; },
      rt_load8_idx: (base, idx) => BigInt(ny.u8(memoryRef)[ny.ptr(base) + ny.int(idx)] || 0),
      rt_store8_idx: (base, idx, val) => { ny.u8(memoryRef)[ny.ptr(base) + ny.int(idx)] = Number(val) & 255; return 0n; },
      rt_store64_idx: (base, idx, val) => { ny.writeI64(memoryRef, ny.ptr(base) + ny.int(idx) * 8, val); return 0n; },
      rt_list_new: (n) => BigInt(ny.list(memoryRef, ny.int(n))),
      rt_list_set_len: (lst, n) => ny.listSetLen(memoryRef, lst, n),
      rt_append: (lst, val) => ny.listAppend(memoryRef, lst, val),
      rt_store_item_fast: (lst, idx, val) => ny.listSet(memoryRef, lst, idx, val),
      rt_load_item: (lst, idx) => ny.listGet(memoryRef, lst, idx, 0n),
      rt_load_item_fast: (lst, idx) => ny.listGet(memoryRef, lst, idx, 0n),
      rt_store_item: (lst, idx, val) => ny.listSet(memoryRef, lst, idx, val),
      __load_item_fast: (lst, idx) => ny.listGet(memoryRef, lst, idx, 0n),
      __store_item_fast: (lst, idx, val) => ny.listSet(memoryRef, lst, idx, val),
      __list_set_len: (lst, n) => ny.listSetLen(memoryRef, lst, n),
      rt_range_new: (start = 1n, stop = 1n, step = 3n) => {
        const r = ny.list(memoryRef, 3);
        ny.listSetLen(memoryRef, r, 3);
        ny.listSet(memoryRef, r, ny.tag(0), start);
        ny.listSet(memoryRef, r, ny.tag(1), stop);
        ny.listSet(memoryRef, r, ny.tag(2), step);
        return BigInt(r);
      },
      rt_tagof: (v) => ny.tag(Number(ny.tagof(memoryRef, v))),
      rt_is_ok: (v) => {
        return ny.bool(ny.tagof(memoryRef, v) === 104n);
      },
      rt_is_err: (v) => ny.bool(ny.tagof(memoryRef, v) === 105n),
      rt_unwrap: (v) => ny.readI64(memoryRef, ny.ptr(v)) || v,
      "std.os.file_read": (path) => {
        const requested = ny.text(memoryRef, path, 0);
        const logical = assetPath(requested);
        const bytes = virtualBytes(logical);
        return bytes ? result(104, ny.string(memoryRef, dec.decode(bytes))) : result(105, ny.tag(-2));
      },
      "std.os.file_write": (path, content) => {
        const logical = assetPath(ny.text(memoryRef, path, 0));
        const bytes = enc.encode(ny.text(memoryRef, content, 0));
        return persistBytes(logical, bytes) ? result(104, ny.tag(bytes.length)) : result(105, ny.tag(-5));
      },
      "std.os.file_exists": (path) => {
        const logical = assetPath(ny.text(memoryRef, path, 0));
        const exists = virtualBytes(logical) !== null || virtualDirectory(logical).length > 0;
        canvas.dataset.vfsExists = exists ? "1" : "0";
        return ny.bool(exists);
      },
      "std.os.file_remove": (path) => {
        const logical = assetPath(ny.text(memoryRef, path, 0));
        const removed = storedBytes(logical) !== null && removePersisted(logical);
        canvas.dataset.vfsRemoved = removed ? "1" : "0";
        return removed ? result(104, ny.tag(0)) : result(105, ny.tag(-2));
      },
      "std.os.file_rename": (oldPath, newPath) => {
        const oldName = assetPath(ny.text(memoryRef, oldPath, 0));
        const newName = assetPath(ny.text(memoryRef, newPath, 0));
        const bytes = storedBytes(oldName);
        if (!bytes || !persistBytes(newName, bytes)) return result(105, ny.tag(-2));
        removePersisted(oldName);
        return result(104, ny.tag(0));
      },
      "std.os.pid": () => ny.tag(1),
      "std.os.fs.is_dir": (path) => {
        const value = virtualPath(path);
        const exists = virtualDirectoryExists(value);
        return ny.bool(exists);
      },
      "std.os.fs.list_dir": (path) => {
        const names = virtualDirectory(virtualPath(path));
        const out = ny.list(memoryRef, names.length);
        for (let i = 0; i < names.length; i++)
          ny.listSet(memoryRef, out, ny.tag(i), ny.string(memoryRef, names[i]));
        ny.listSetLen(memoryRef, out, names.length);
        return out;
      },
      "std.os.fs.make_dir": (path) => {
        const logical = virtualPath(path).replace(/^\/+|\/+$/g, "");
        if (!logical || virtualDirectoryExists(logical)) {
          return result(105, ny.tag(-17));
        }
        const slash = logical.lastIndexOf("/");
        const parent = slash > 0 ? logical.slice(0, slash) : ".";
        const made = virtualDirectoryExists(parent) && persistDir(logical);
        return made ? result(104, ny.tag(0)) : result(105, ny.tag(-2));
      },
      "std.os.fs.remove_dir": (path) => {
        const logical = virtualPath(path).replace(/^\/+|\/+$/g, "");
        if (!logical || !storedDir(logical)) {
          return result(105, ny.tag(-2));
        }
        if (virtualDirectory(logical).length) {
          return result(105, ny.tag(-39));
        }
        const removed = removePersistedDir(logical);
        return removed ? result(104, ny.tag(0)) : result(105, ny.tag(-5));
      },
      rt_str_concat: (a, b) => ny.string(memoryRef, ny.valueToString(memoryRef, a) + ny.valueToString(memoryRef, b)),
      rt_to_str: (v) => ny.string(memoryRef, ny.valueToString(memoryRef, v)),
      rt_add: (a, b) => {
        const f = ny.isFloat(memoryRef, a) || ny.isFloat(memoryRef, b);
        return ny.numericResult(memoryRef, ny.numeric(memoryRef, a) + ny.numeric(memoryRef, b), f);
      },
      rt_sub: (a, b) => {
        const f = ny.isFloat(memoryRef, a) || ny.isFloat(memoryRef, b);
        return ny.numericResult(memoryRef, ny.numeric(memoryRef, a) - ny.numeric(memoryRef, b), f);
      },
      rt_mul: (a, b) => {
        const f = ny.isFloat(memoryRef, a) || ny.isFloat(memoryRef, b);
        return ny.numericResult(memoryRef, ny.numeric(memoryRef, a) * ny.numeric(memoryRef, b), f);
      },
      rt_lt: (a, b) => ny.bool(ny.numeric(memoryRef, a) < ny.numeric(memoryRef, b)),
      rt_eq: (a, b) => {
        return ny.bool(ny.equal(memoryRef, a, b));
      },
      rt_has_tag: (v, tag) => {
        const want = BigInt.asIntN(64, BigInt(tag || 0));
        if (want === 110n) return ny.bool(ny.isFloat(memoryRef, v));
        if (want === 121n && ny.tagof(memoryRef, v) === 120n) return NY_TRUE;
        return ny.bool(ny.tagof(memoryRef, v) === want);
      },
      rt_is_float_obj: (v) => ny.bool(ny.isFloat(memoryRef, v)),
      rt_bigint_from_str: (v) => ny.tag(parseInt(ny.text(memoryRef, v, 0), 10) || 0),
      rt_bigint_to_int: (v) => v,
      rt_fix_fn_ptr: (v) => v,
      // Browser apps do not unwind their top-level frame loop. Native defer
      // callbacks only run during that unwind, so this host keeps the stack
      // entry inert until browser lifecycle cleanup is implemented.
      rt_push_defer: () => 0n,
      rt_trace_loc: () => 0n,
      rt_flt_box_val: (bits) => ny.fltBox(memoryRef, bits),
      rt_flt_box_val32: (bits32) => {
        const buf = new ArrayBuffer(4);
        const view = new DataView(buf);
        view.setUint32(0, Number(ny.int(bits32)) >>> 0, true);
        return ny.fltFromNumber(memoryRef, view.getFloat32(0, true));
      },
      rt_flt_unbox_val: (v) => ny.fltBits(memoryRef, v),
      rt_flt_unbox_val32: (v) => {
        const buf = new ArrayBuffer(4);
        const view = new DataView(buf);
        view.setFloat32(0, ny.flt(memoryRef, v), true);
        return ny.tag(view.getUint32(0, true));
      },
      rt_flt_from_int: (v) => ny.fltFromNumber(memoryRef, ny.int(v)),
      rt_flt_to_int: (v) => ny.tag(Math.trunc(ny.flt(memoryRef, v))),
      rt_flt_trunc: (v) => ny.tag(Math.trunc(ny.flt(memoryRef, v))),
      "std.core.pow": (a, b) => ny.fltFromNumber(memoryRef, Math.pow(ny.numeric(memoryRef, a), ny.numeric(memoryRef, b))),
      rt_print_int: (v) => { appendStdout(String(ny.int(v))); return v; },
      rt_print_str_raw: (v) => { appendStdout(ny.valueToString(memoryRef, v)); return v; },
      rt_print_newline: () => { flushStdout(); return 1n; },
      rt_panic: () => { resetBrowserBoundary(); throw new Error("Ny wasm panic"); },
      "std.core.panic": (message = 0n) => { resetBrowserBoundary(); throw new Error("Ny wasm panic: " + ny.text(memoryRef, message, 0)); },
      rt_os_name: () => ny.string(memoryRef, "web"),
      rt_arch_name: () => ny.string(memoryRef, "wasm32"),
      __os_name: () => ny.string(memoryRef, "web"),
      __arch_name: () => ny.string(memoryRef, "wasm32"),
      "std.os.exit": () => 0n,
      "std.os.time.ticks": () => ny.tag(Math.trunc(performance.now() * 1000000)),
      __time_seconds: () => ny.tag(Math.floor(Date.now() / 1000)),
      __time_milliseconds: () => ny.tag(Date.now()),
      __ticks_ns: () => ny.tag(Math.floor(performance.now() * 1000000)),
      "std.os.time.now": () => ny.tag(Math.floor(Date.now() / 1000)),
      "std.os.time.unix": () => ny.tag(Math.floor(Date.now() / 1000)),
      "std.os.time.now_ms": () => ny.tag(Date.now()),
      "std.os.time.monotonic_ns": () => ny.tag(Math.floor(performance.now() * 1000000)),
      "std.os.time.msleep": (ms = 0n) => {
        if (!asyncifyRef.controller)
          throw new Error("std.os.time.msleep requires Asyncify instrumentation");
        asyncifyRef.controller.sleep(ny.int(ms));
        return 0n;
      },
      "std.os.fetch": webFetch,
      __web_fetch: webFetch,
      "std.os.clipboard.write": webClipboardWrite,
      "std.os.clipboard.read": webClipboardRead,
      "std.os.set_clipboard_text": webClipboardWrite,
      "std.os.get_clipboard_text": webClipboardRead,
      __web_clipboard_write: webClipboardWrite,
      __web_clipboard_read: webClipboardRead,
      "std.os.websocket.open": websocketOpen,
      "std.os.websocket.state": websocketState,
      "std.os.websocket.send": websocketSend,
      "std.os.websocket.receive": websocketReceive,
      "std.os.websocket.close": websocketClose,
      __websocket_open: websocketOpen,
      __websocket_state: websocketState,
      __websocket_send: websocketSend,
      __websocket_receive: websocketReceive,
      __websocket_close: websocketClose,
      "std.os.args.first_positive_int": (fallback = 0n) => {
        for (const arg of refreshRunArgv(meta).slice(3)) {
          const n = parseInt(arg, 10);
          if (Number.isFinite(n) && n > 0) return ny.tag(n);
        }
        return fallback;
      },
      "std.os.prim.env": () => ny.string(memoryRef, ""),
      "std.os.prim.os": () => ny.string(memoryRef, "web"),
      "std.os.os": () => ny.string(memoryRef, "web"),
      "std.os.sound.init": () => {
        const context = ensureAudio();
        return context ? NY_TRUE : NY_FALSE;
      },
      "std.os.sound.shutdown": () => {
        shutdownAudio();
        return 0n;
      },
      "std.os.sound.get_backend_name": () => ny.string(memoryRef, ensureAudio() ? "web-audio" : "none"),
      "std.os.sound.web_play": (path) => {
        const key = ny.text(memoryRef, path, 0);
        const bytes = assetCache.get(key);
        const context = ensureAudio();
        if (!bytes || !context) {
          canvas.setAttribute("data-audio-decode", "0");
          return NY_FALSE;
        }
        const startSource = (buffer) => {
          canvas.setAttribute("data-audio-decode", "1");
          canvas.setAttribute("data-audio-decode-length", String(buffer.length));
          canvas.setAttribute("data-audio-decode-duration", String(Math.round(buffer.duration * 1000)));
          const source = context.createBufferSource();
          source.buffer = buffer;
          source.connect(context.destination);
          source.start(0);
          canvas.setAttribute("data-audio-source-started", "1");
          if (context.state === "suspended") context.resume().catch(() => {});
        };
        const wav = decodeWavPcm(context, bytes);
        if (wav) {
          startSource(wav);
          return NY_TRUE;
        }
        context.decodeAudioData(bytes.slice().buffer)
          .then(startSource)
          .catch(() => canvas.setAttribute("data-audio-decode", "0"));
        return NY_TRUE;
      },
      "std.core.dict_mod.dict": () => 0n,
      "std.os.args.args": () => {
        const argv = refreshRunArgv(meta);
        const lst = ny.list(memoryRef, argv.length);
        ny.listSetLen(memoryRef, lst, argv.length);
        argv.forEach((arg, i) => ny.listSet(memoryRef, lst, ny.tag(i), ny.string(memoryRef, arg)));
        return lst;
      },
      "std.core.any.len": (v) => ny.tag(ny.listLen(memoryRef, v) || ny.text(memoryRef, v, 0).length),
      "std.core.get": (lst, idx, fallback = 0n) => ny.listGet(memoryRef, lst, idx, fallback),
      "std.core.any.get": (lst, idx, fallback = 0n) => ny.listGet(memoryRef, lst, idx, fallback),
      "std.core.index_read": (lst, idx) => ny.listGet(memoryRef, lst, idx, 0n),
      "std.core.range.len": (r) => {
        const start = ny.int(ny.listGet(memoryRef, r, ny.tag(0), ny.tag(0)));
        const stop = ny.int(ny.listGet(memoryRef, r, ny.tag(1), ny.tag(0)));
        const step = Math.max(1, ny.int(ny.listGet(memoryRef, r, ny.tag(2), ny.tag(1))));
        return ny.tag(Math.max(0, Math.ceil((stop - start) / step)));
      },
      "std.core.malloc": (size) => BigInt(ny.alloc(memoryRef, ny.int(size))),
      "std.core.free": () => 0n,
      "std.core.assert": (cond, msg = 0n) => {
        if (BigInt(cond || 0) === NY_FALSE || BigInt(cond || 0) === 0n) throw new Error("assert failed: " + ny.text(memoryRef, msg, 0));
        return cond;
      },
      "std.core.assert_eq": (a, b, msg = 0n) => {
        if (BigInt(a || 0) !== BigInt(b || 0) && ny.numeric(memoryRef, a) !== ny.numeric(memoryRef, b)) {
          throw new Error("assert_eq failed: " + ny.text(memoryRef, msg, 0));
        }
        return a;
      },
      "std.core.eq": (a, b) => ny.bool(ny.equal(memoryRef, a, b)),
      "std.core.contains": (container, item) => {
        const containerTag = ny.tagof(memoryRef, container);
        const itemTag = ny.tagof(memoryRef, item);
        if ((containerTag === 120n || containerTag === 121n) &&
            (itemTag === 120n || itemTag === 121n)) {
          return ny.bool(ny.text(memoryRef, container, 0).includes(ny.text(memoryRef, item, 0)));
        }
        return NY_FALSE;
      },
      "std.core.lt": (a, b) => ny.bool(ny.numeric(memoryRef, a) < ny.numeric(memoryRef, b)),
      "std.core.le": (a, b) => ny.bool(ny.numeric(memoryRef, a) <= ny.numeric(memoryRef, b)),
      "std.core.ge": (a, b) => ny.bool(ny.numeric(memoryRef, a) >= ny.numeric(memoryRef, b)),
      "std.core.gt": (a, b) => ny.bool(ny.numeric(memoryRef, a) > ny.numeric(memoryRef, b)),
      "std.math.max": (a, b) => ny.numericResult(memoryRef, Math.max(ny.numeric(memoryRef, a), ny.numeric(memoryRef, b)), ny.isFloat(memoryRef, a) || ny.isFloat(memoryRef, b)),
      "std.math.min": (a, b) => ny.numericResult(memoryRef, Math.min(ny.numeric(memoryRef, a), ny.numeric(memoryRef, b)), ny.isFloat(memoryRef, a) || ny.isFloat(memoryRef, b)),
      "std.math.abs": (a) => ny.numericResult(memoryRef, Math.abs(ny.numeric(memoryRef, a)), ny.isFloat(memoryRef, a)),
      "std.math.clamp": (v, lo, hi) => ny.numericResult(memoryRef, Math.min(Math.max(ny.numeric(memoryRef, v), ny.numeric(memoryRef, lo)), ny.numeric(memoryRef, hi)), ny.isFloat(memoryRef, v) || ny.isFloat(memoryRef, lo) || ny.isFloat(memoryRef, hi)),
      "std.math.lerp": (a, b, t) => ny.numericResult(memoryRef, ny.numeric(memoryRef, a) + (ny.numeric(memoryRef, b) - ny.numeric(memoryRef, a)) * ny.numeric(memoryRef, t), ny.isFloat(memoryRef, a) || ny.isFloat(memoryRef, b) || ny.isFloat(memoryRef, t)),
      "std.core.is_str": (v) => {
        const tag = ny.tagof(memoryRef, v);
        return ny.bool(tag === 120n || tag === 121n);
      },
      "std.core.atoi": (v) => ny.tag(parseInt(ny.text(memoryRef, v, 0), 10) || 0),
      "std.core.to_str": (v) => ny.string(memoryRef, ny.valueToString(memoryRef, v)),
      "std.core.reflect.to_str": (v) => ny.string(memoryRef, ny.valueToString(memoryRef, v)),
      "std.core.reflect.div": (a, b) => ny.tag(Math.trunc(ny.numeric(memoryRef, a) / Math.max(1, ny.numeric(memoryRef, b)))),
      "std.core.reflect.eq": (a, b) => ny.bool(ny.equal(memoryRef, a, b)),
      "std.core.type": (v) => ny.tag(Number(ny.tagof(memoryRef, v))),
      "std.core.ok": (v) => v,
      "std.core.term.write_str": () => 0n,
      "std.core.term.color": (s) => s || 0n,
      "std.core.term.tui_begin": () => 0n,
      "std.core.term.tui_end": () => 0n,
      "std.core.term.poll_key": () => ny.tag(0),
      "std.core.term.is_quit_key": () => ny.tag(0),
      "std.core.term.canvas": () => 0n,
      "std.core.term.canvas_clear": () => 0n,
      "std.core.term.canvas_set": () => 0n,
      "std.core.term.canvas_refresh": () => 0n,
      "std.core.term.get_terminal_size": () => 0n,
      __multi3: (out, aLo, aHi, bLo, bHi) => {
        const mask = (1n << 64n) - 1n;
        const a = (BigInt.asIntN(64, BigInt(aHi || 0)) << 64n) | (BigInt(aLo || 0) & mask);
        const b = (BigInt.asIntN(64, BigInt(bHi || 0)) << 64n) | (BigInt(bLo || 0) & mask);
        const product = BigInt.asIntN(128, a * b);
        ny.writeI64(memoryRef, Number(out), BigInt.asIntN(64, product & mask));
        ny.writeI64(memoryRef, Number(out) + 8, BigInt.asIntN(64, product >> 64n));
        return 0n;
      },
    };
  }

  function makeImports(meta, module, memoryRef) {
    const asyncifyRef = { controller: null, closed: false };
    const host = { ...makeRuntimeImports(meta, memoryRef, asyncifyRef), ...makeWebImports(meta, memoryRef), ...makeUiImports(memoryRef, asyncifyRef) };
    const imports = {};
    for (const imp of WebAssembly.Module.imports(module)) {
      if (!imports[imp.module]) imports[imp.module] = {};
      if (imp.kind === "memory") imports[imp.module][imp.name] = memoryRef.memory;
      else if (imp.kind === "table") imports[imp.module][imp.name] = new WebAssembly.Table({ initial: 0, element: "anyfunc" });
      else if (imp.kind === "global") imports[imp.module][imp.name] = new WebAssembly.Global({ value: "i64", mutable: true }, 0n);
      else if (imp.kind === "function") imports[imp.module][imp.name] = host[imp.name] || (() => { throw new Error(`${meta.id}: unsupported import ${imp.module}.${imp.name}`); });
    }
    return { imports, asyncifyRef };
  }

  /*
   * Headless-Chromium self-test for the touch host imports.
   *
   * The browser test harness runs `chromium --dump-dom` with no CDP, so it
   * cannot synthesize input events itself. Instead, when the page is loaded
   * with `#touch-selftest`, this routine dispatches a real TouchEvent
   * sequence on the canvas after the app starts; the fixture reads the touch
   * state through the public input facade and echoes it back via
   * `std.os.ui.window.test_report_touch` into data-touch-* attributes, which
   * the harness greps out of --dump-dom.
   */
  function scheduleTouchSelftest(canvas) {
    if (!canvas) return;
    const r = canvas.getBoundingClientRect();
    /* Map a logical stage point (e.g. stage 80,60) back to a CSS client point
       that updateTouches() will scale back to the same stage coordinates. */
    const sx = r.width / Math.max(1, stage.width);
    const sy = r.height / Math.max(1, stage.height);
    const mk = (type, id, stageX, stageY) => {
      const cx = r.left + stageX * sx;
      const cy = r.top + stageY * sy;
      const touch = window.Touch
        ? new Touch({ identifier: id, target: canvas, clientX: cx, clientY: cy })
        : { identifier: id, target: canvas, clientX: cx, clientY: cy };
      if (window.TouchEvent && window.Touch) {
        return new TouchEvent(type, { cancelable: true, bubbles: true, changedTouches: [touch], touches: [touch] });
      }
      /* Headless Chromium currently exposes TouchEvent without exposing the
         Touch constructor.  The host only consumes the iterable touch-list
         fields, so an Event with standards-shaped properties is sufficient
         and keeps this self-test independent of device emulation. */
      const event = new Event(type, { cancelable: true, bubbles: true });
      Object.defineProperty(event, "changedTouches", { value: [touch] });
      Object.defineProperty(event, "touches", { value: [touch] });
      return event;
    };
    /* The fixture stops the loop as soon as it observes active touch and
       echoes the observed count, so keep the touch held rather than racing a
       quick touchend: any frame after touchstart reliably sees count > 0. */
    setTimeout(() => {
      canvas.dispatchEvent(mk("touchstart", 7, 80, 60));
      canvas.dispatchEvent(mk("touchmove", 7, 90, 70));
      setTimeout(() => canvas.dispatchEvent(mk("touchend", 7, 90, 70)), 4600);
      canvas.dataset.touchSelftest = "1";
    }, 0);
  }

  /* Inject a synthetic standard-mapped Gamepad so the headless self-test can
     prove the mapping code: connect one pad, press button 0 ("A") and move the
     left stick, which the fixture echoes back via test_report_gamepad. */
  function scheduleGamepadSelftest() {
    window.__nyFakeGamepads = () => [null, {
      id: "NY-Fake-Gamepad",
      index: 1,
      connected: true,
      mapping: "standard",
      touch: true,
      axes: [0.5, -0.25, 0, 0],
      buttons: Array.from({ length: 16 }, (_, i) => ({ pressed: i === 0, touched: i === 0, value: i === 0 ? 1.0 : 0.0 })),
    }];
  }

  async function loadRuntime(meta) {
    const token = ++runtimeToken;
    currentRuntime = null;
    setStageMode(wantsCliStage(meta, null) ? "cli" : "web");
    refreshCliStage();
    setKernelStatus();
    if (!meta.wasm && !meta.wasmBytes && !meta.wasmBase64) {
      resetOutput([meta.title || meta.id, "artifact missing", "Build with ./make wasm or load a local file."]);
      if (wantsCliStage(meta, null)) refreshCliStage();
      else drawStatusSurface("Artifact missing", [meta.title || meta.id, "Build with ./make wasm or load a local file."]);
      return;
    }
    try {
      await preloadAssets(meta);
      if (token !== runtimeToken || currentMeta !== meta) return;
      const bytes = meta.wasmBytes || (meta.wasmBase64 ? (() => {
        const raw = atob(meta.wasmBase64);
        const b = new Uint8Array(raw.length);
        for (let i = 0; i < raw.length; i++) b[i] = raw.charCodeAt(i);
        return b;
      })() : await (await fetch(meta.wasm)).arrayBuffer());

      const module = await WebAssembly.compile(bytes);
      const memoryRef = { memory: fallbackMemory, heapTop: 1048576 };
      const importState = makeImports(meta, module, memoryRef);
      const instance = await WebAssembly.instantiate(module, importState.imports);
      if (token !== runtimeToken || currentMeta !== meta) return;
      memoryRef.memory = instance.exports.memory || memoryRef.memory;
      const entry = selectedEntry(instance.exports);
      const browserEntry = Boolean(meta.asyncify) || ["ny_web_frame", "ny_web_render", "ny_web_main"].some(n => typeof instance.exports[n] === "function");
      const oneShot = !browserEntry;
      currentRuntime = { id: meta.id, exports: instance.exports, memory: memoryRef.memory, entry, ran: false, oneShot, asyncify: null };
      if (meta.asyncify) {
        if (!entry || typeof instance.exports.asyncify_get_state !== "function") throw new Error(`${meta.id}: asyncify metadata does not match this Wasm module`);
        currentRuntime.asyncify = makeAsyncify(instance, memoryRef, () => instance.exports[entry]());
        importState.asyncifyRef.controller = currentRuntime.asyncify;
      }
      setStageMode(oneShot ? "cli" : "web");
      setKernelStatus();
      resetOutput(runtimeHeader(meta, currentRuntime));
      running = autoRunInput && autoRunInput.checked;
      if ($("runBtn")) $("runBtn").textContent = running && !oneShot ? "Pause" : "Run";
      if (typeof instance.exports.ny_web_init === "function") instance.exports.ny_web_init(stage.width, stage.height);
      if (frameTouched && !framePresented) present();
      if (!frameTouched && oneShot) refreshCliStage();
      else if (!frameTouched) drawStatusSurface("Web Ready", [meta.title, `entry: ${entry}`]);
      if (window.location.hash === "#touch-selftest") scheduleTouchSelftest(canvas);
      if (window.location.hash === "#gamepad-selftest") scheduleGamepadSelftest();
      if (running) setTimeout(() => runFrame(0.016), 0);
    } catch (err) {
      if (token !== runtimeToken || currentMeta !== meta) return;
      resetBrowserBoundary();
      setKernelStatus("Load failed", "warn");
      resetOutput([meta.id, "error", err.message]);
      if (wantsCliStage(meta, currentRuntime)) refreshCliStage();
      else drawStatusSurface("Load failed", [meta.id, err.message]);
    }
  }

  function runFrame(dt) {
    if (!currentRuntime || !currentMeta || currentRuntime.id !== currentMeta.id) return;
    const ex = currentRuntime.exports;
    try {
      frameTouched = false;
      framePresented = false;
      if (currentRuntime.asyncify) {
        if (!currentRuntime.ran && currentRuntime.entry) {
          currentRuntime.ran = true;
          ex[currentRuntime.entry]();
        }
        const scheduleAsyncResume = (baseDt) => {
          if (!currentRuntime || !currentRuntime.asyncify) return;
          const delay = currentRuntime.asyncify.resumeDelay();
          const resume = (ts) => {
            if (!currentRuntime || !currentRuntime.asyncify) return;
            const nextDt = lastTime ? Math.min(0.05, (ts - lastTime) / 1000) : baseDt;
            lastTime = ts;
            currentRuntime.asyncify.resume(nextDt);
            if (currentRuntime.asyncify.needsResume()) {
              currentRuntime.asyncify.finishUnwind();
              scheduleAsyncResume(nextDt);
            }
          };
          if (delay === Infinity) {
            currentRuntime.asyncify.waitForResume((ts) => resume(ts));
            return;
          }
          if (delay > 0) setTimeout(() => resume(performance.now()), delay);
          else requestAnimationFrame(resume);
        };
        if (currentRuntime.asyncify.needsResume()) {
          currentRuntime.asyncify.finishUnwind();
          scheduleAsyncResume(dt);
        }
      } else if (typeof ex.ny_web_frame === "function") ex.ny_web_frame(Number(dt), stage.width, stage.height);
      else if (typeof ex.ny_web_render === "function") ex.ny_web_render(stage.width, stage.height);
      else if (!currentRuntime.ran && typeof ex.ny_web_main === "function") { currentRuntime.ran = true; ex.ny_web_main(); }
      else if (!currentRuntime.ran && currentRuntime.entry && typeof ex[currentRuntime.entry] === "function") {
        currentRuntime.ran = true;
        if (currentRuntime.oneShot) {
          resetOutput(runtimeHeader(currentMeta, currentRuntime));
          appendLog("running...");
        }
        ex[currentRuntime.entry]();
        if (currentRuntime.oneShot) drawOutputSurface(currentMeta.title);
      }
      if (frameTouched && !framePresented) present();
    } catch (err) {
      console.error(err && err.stack ? err.stack : err);
      resetBrowserBoundary();
      currentRuntime = null;
      setKernelStatus("Error", "warn");
      resetOutput([currentMeta.id, "runtime error", err.stack || err.message]);
      if (wantsCliStage(currentMeta, currentRuntime)) refreshCliStage();
      else drawStatusSurface("Runtime Error", [err.message]);
    }
  }

  function renderControls() {
    controls.innerHTML = "";
    ["Run once", "Reload"].forEach(label => {
      const btn = document.createElement("button");
      btn.textContent = label;
      btn.addEventListener("click", () => {
        if (label === "Reload") loadRuntime(currentMeta);
        else { if (currentRuntime) currentRuntime.ran = false; runFrame(0.016); }
      });
      controls.appendChild(btn);
    });
  }

  function selectDemo(id, writeHash = true) {
    const meta = demos.find((d) => d.id === id) || demos[0];
    if (!meta) return;
    if (writeHash) history.replaceState(null, "", `#${meta.id}`);
    currentMeta = meta;
    setStageMode(wantsCliStage(meta, null) ? "cli" : "web");
    refreshRunArgv(meta);
    $("demoArea").textContent = meta.mode;
    $("demoTitle").textContent = meta.title;
    $("demoSource").textContent = meta.source || "local file";
    document.querySelectorAll(".demo-item").forEach((b) => b.classList.toggle("active", b.dataset.id === meta.id));
    resetOutput(runtimeHeader(meta, null));
    renderControls();
    loadRuntime(meta);
  }

  async function loadLocalWasm(file) {
    if (!file || !/\.wasm$/i.test(file.name || "")) {
      canvas.dataset.localWasm = "rejected";
      return false;
    }
    try {
      const meta = {
        id: "local-" + file.name,
        title: file.name,
        source: "local file",
        area: "LOCAL",
        mode: "native",
        wasmBytes: await file.arrayBuffer(),
      };
      const existing = demos.findIndex((demo) => demo.id === meta.id);
      if (existing >= 0) demos[existing] = meta;
      else demos.push(meta);
      renderList();
      selectDemo(meta.id, false);
      canvas.dataset.localWasm = "loaded";
      return true;
    } catch (err) {
      canvas.dataset.localWasm = "error";
      resetOutput(["local wasm", "error", err && err.message ? err.message : String(err)]);
      return false;
    }
  }

  function renderAreas() {
    const areas = ["All", ...Array.from(new Set(demos.map((d) => d.area)))];
    $("areaTabs").innerHTML = areas.map(a => `<button class="area-tab" data-area="${esc(a)}">${esc(a)}</button>`).join("");
    $("areaTabs").querySelectorAll("button").forEach(b => b.addEventListener("click", () => {
      selectedArea = b.dataset.area;
      $("areaTabs").querySelectorAll("button").forEach(btn => btn.classList.toggle("active", btn.dataset.area === selectedArea));
      renderList();
    }));
    const all = $("areaTabs").querySelector('button[data-area="All"]');
    if (all) all.classList.add("active");
  }

  function renderList() {
    const items = demos.filter((d) => selectedArea === "All" || d.area === selectedArea);
    demoItems.innerHTML = items.map(d => `<button class="demo-item" data-id="${esc(d.id)}"><strong>${esc(d.title)}</strong><span>${esc(d.source || "wasm")}</span></button>`).join("");
    demoItems.querySelectorAll(".demo-item").forEach(b => b.addEventListener("click", () => selectDemo(b.dataset.id)));
    if (currentMeta) document.querySelectorAll(".demo-item").forEach(b => b.classList.toggle("active", b.dataset.id === currentMeta.id));
  }

  function loop(ts) {
    const dt = lastTime ? Math.min(0.05, (ts - lastTime) / 1000) : 0.016;
    lastTime = ts;
    if (running && (!currentRuntime || (!currentRuntime.oneShot && !currentRuntime.asyncify))) runFrame(dt);
    requestAnimationFrame(loop);
  }

  const resizeTarget = $("stagePane") || canvas;
  if (typeof ResizeObserver === "function") {
    new ResizeObserver(() => fitCanvas()).observe(resizeTarget);
  } else {
    window.addEventListener("resize", fitCanvas);
  }
  window.addEventListener("orientationchange", fitCanvas);
  window.addEventListener("hashchange", () => selectDemo(window.location.hash.slice(1), false));
  window.addEventListener("keydown", (e) => {
    const code = e.keyCode || e.which || 0;
    input.key = e.key;
    if (!input.codes.has(code)) input.pressed.add(code);
    input.codes.add(code);
    resumeAudio();
  });
  window.addEventListener("keyup", (e) => { input.codes.delete(e.keyCode || e.which || 0); });
  window.addEventListener("blur", clearInput);
  document.addEventListener("visibilitychange", () => {
    const visible = !document.hidden;
    canvas.dataset.visible = visible ? "1" : "0";
    if (!visible) clearInput();
  });
  document.addEventListener("fullscreenchange", refreshBrowserRequestState);
  document.addEventListener("pointerlockchange", refreshBrowserRequestState);
  canvas.dataset.visible = document.hidden ? "0" : "1";
  function updatePointer(e) {
    const r = canvas.getBoundingClientRect();
    input.mouse = [
      (e.clientX - r.left) * stage.width / Math.max(1, r.width),
      (e.clientY - r.top) * stage.height / Math.max(1, r.height),
    ];
  }
  canvas.addEventListener("mousemove", updatePointer);
  canvas.addEventListener("mousedown", (e) => {
    updatePointer(e);
    if (!input.buttons.has(e.button)) input.pressedButtons.add(e.button);
    input.buttons.add(e.button);
    resumeAudio();
  });
  canvas.addEventListener("wheel", (e) => {
    updatePointer(e);
    input.scroll[0] += e.deltaX;
    input.scroll[1] += e.deltaY;
  }, { passive: true });
  /* Touch input: map to logical stage coordinates, same scaling as the mouse. */
  function updateTouches(e) {
    const r = canvas.getBoundingClientRect();
    const sx = stage.width / Math.max(1, r.width);
    const sy = stage.height / Math.max(1, r.height);
    for (const t of e.changedTouches) {
      input.touches.set(t.identifier, [(t.clientX - r.left) * sx, (t.clientY - r.top) * sy]);
    }
  }
  canvas.addEventListener("touchstart", (e) => {
    resumeAudio();
    updateTouches(e);
    for (const t of e.changedTouches) input.touchStarts.add(t.identifier);
  }, { passive: true });
  canvas.addEventListener("touchmove", (e) => { updateTouches(e); }, { passive: true });
  canvas.addEventListener("touchend", (e) => {
    updateTouches(e);
    for (const t of e.changedTouches) input.touches.delete(t.identifier);
  }, { passive: true });
  canvas.addEventListener("touchcancel", (e) => {
    for (const t of e.changedTouches) input.touches.delete(t.identifier);
  }, { passive: true });
  window.addEventListener("mouseup", (e) => { input.buttons.delete(e.button); });
  window.addEventListener("gamepadconnected", refreshGamepadStatus);
  window.addEventListener("gamepaddisconnected", refreshGamepadStatus);

  wasmFile.addEventListener("change", () => loadLocalWasm(wasmFile.files[0]));
  const stagePane = $("stagePane");
  if (stagePane) {
    stagePane.addEventListener("dragover", (event) => {
      if (event.dataTransfer && Array.from(event.dataTransfer.items || []).some((item) => item.kind === "file")) {
        event.preventDefault();
        event.dataTransfer.dropEffect = "copy";
        stagePane.classList.add("drop-active");
      }
    });
    stagePane.addEventListener("dragleave", (event) => {
      if (!stagePane.contains(event.relatedTarget)) stagePane.classList.remove("drop-active");
    });
    stagePane.addEventListener("drop", (event) => {
      event.preventDefault();
      stagePane.classList.remove("drop-active");
      const file = Array.from(event.dataTransfer && event.dataTransfer.files || [])
        .find((candidate) => /\.wasm$/i.test(candidate.name || ""));
      loadLocalWasm(file);
    });
  }

  $("runBtn").addEventListener("click", () => {
    running = !running;
    if (autoRunInput) autoRunInput.checked = running;
    const isOneShot = currentRuntime && currentRuntime.oneShot;
    $("runBtn").textContent = running && !isOneShot ? "Pause" : "Run";
    if (running && isOneShot) { currentRuntime.ran = false; runFrame(0.016); }
  });

  if (autoRunInput) autoRunInput.addEventListener("change", () => {
    running = autoRunInput.checked;
    const isOneShot = currentRuntime && currentRuntime.oneShot;
    $("runBtn").textContent = running && !isOneShot ? "Pause" : "Run";
  });

  $("clearBtn").addEventListener("click", () => resetOutput(currentMeta ? runtimeHeader(currentMeta, currentRuntime) : []));
  $("resetBtn").addEventListener("click", () => { if (currentMeta) loadRuntime(currentMeta); });

  window.NYTRIX_WASM_DEBUG = {
    current: () => currentMeta,
    runtime: () => currentRuntime,
    stats: () => ({
      stage: [stage.width, stage.height],
      canvas: [canvas.width, canvas.height],
      current: currentMeta ? currentMeta.id : null,
      presentCount
    })
  };

  renderAreas();
  renderList();
  initGL();
  refreshGamepadStatus();
  refreshAudioStatus();
  refreshBrowserRequestState();
  selectDemo(window.location.hash.slice(1) || (demos[0] && demos[0].id), false);
  requestAnimationFrame(loop);
})();
