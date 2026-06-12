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
  const fallbackMemory = new WebAssembly.Memory({ initial: 256, maximum: 1024 });
  const input = { key: "-", code: 0, mouse: [0, 0], down: false };

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

  function setStageSize(w = 1280, h = 720) {
    if (stage.width !== w) stage.width = w;
    if (stage.height !== h) stage.height = h;
    ctx.setTransform(1, 0, 0, 1, 0, 0);
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
    ctx.font = "650 30px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace";
    ctx.fillText(title, 132, 128);
    // branding
    ctx.font = "700 12px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace";
    ctx.textAlign = "right";
    ctx.fillStyle = "rgba(147, 160, 155, 0.35)";
    ctx.fillText("NYTRIX", 1184, 114);
    ctx.textAlign = "left";
    // lines
    ctx.font = "15px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace";
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
    ctx.font = "650 25px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace";
    ctx.fillText(title, 132, 126);
    // branding
    ctx.font = "700 12px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace";
    ctx.textAlign = "right";
    ctx.fillStyle = "rgba(147, 160, 155, 0.35)";
    ctx.fillText("NYTRIX", 1184, 114);
    ctx.textAlign = "left";
    ctx.font = "13px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace";
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
  }

  function shader(type, source) {
    const s = gl.createShader(type);
    gl.shaderSource(s, source);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
    return s;
  }

  function initGL() {
    gl = canvas.getContext("webgl2", { alpha: false, antialias: false, preserveDrawingBuffer: true }) ||
         canvas.getContext("webgl", { alpha: false, antialias: false, preserveDrawingBuffer: true });
    if (!gl) { setStatus("webglStatus", "WebGL missing", "warn"); return false; }
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
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    const texLoc = gl.getUniformLocation(program, "tex");
    if (texLoc) gl.uniform1i(texLoc, 0);
    const webgl2 = typeof WebGL2RenderingContext !== "undefined" && gl instanceof WebGL2RenderingContext;
    setStatus("webglStatus", webgl2 ? "WebGL2" : "WebGL", "ready");
    fitCanvas();
    return true;
  }

  function present() {
    setStageMode("web");
    if (!ctx) return;
    canvas.style.backgroundImage = `url("${stage.toDataURL("image/png")}")`;
    canvas.style.backgroundSize = "100% 100%";
    canvas.style.backgroundRepeat = "no-repeat";
    if (!gl || gl.isContextLost()) return;
    fitCanvas();
    try {
      gl.useProgram(program);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, tex);
      const frame = ctx.getImageData(0, 0, stage.width, stage.height);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, stage.width, stage.height, 0, gl.RGBA, gl.UNSIGNED_BYTE, frame.data);
      gl.clearColor(0, 0, 0, 1);
      gl.clear(gl.COLOR_BUFFER_BIT);
      gl.drawArrays(gl.TRIANGLES, 0, 6);
      presentCount++;
    } catch (_) { setStatus("webglStatus", "WebGL lost", "warn"); }
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
      if (v === 0n) return "none";
      if (v === NY_TRUE) return "true";
      if (v === NY_FALSE) return "false";
      const tag = ny.tagof(memoryRef, v);
      if (tag === 120n || tag === 121n) return ny.text(memoryRef, v, 0);
      if (tag === 100n) return `[list ${ny.listLen(memoryRef, v)}]`;
      if (ny.isFloat(memoryRef, v)) return String(ny.flt(memoryRef, v));
      if ((v & 1n) === 1n) return String(Number(v >> 1n));
      return String(Number(v));
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
      ny_web_key_down: (code) => ny.tag(input.code === Number(code) ? 1 : 0),
      ny_web_mouse_down: () => ny.tag(input.down ? 1 : 0),
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
        ctx.font = `${Math.max(8, Number(size) || 14)}px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace`;
        ctx.textBaseline = "top";
        ctx.fillText(ny.text(memoryRef, ptr, len), Number(x), Number(y));
        return 0n;
      },
      ny_web_log: (ptr, len) => resetOutput([meta.source, ny.text(memoryRef, ptr, len)]),
      ny_web_present: () => { framePresented = true; present(); return 0n; },
    };
  }

  function makeRuntimeImports(meta, memoryRef) {
    return {
      rt_argc: () => ny.tag(refreshRunArgv(meta).length),
      "std.core.primitives.argc": () => ny.tag(refreshRunArgv(meta).length),
      rt_argv: () => 0n,
      rt_runtime_tag: (v) => v,
      rt_malloc: (size) => ny.alloc(memoryRef, ny.int(size)),
      rt_realloc: (_ptr, size) => ny.alloc(memoryRef, ny.int(size)),
      rt_free: () => 0n,
      rt_memset: (dst, val, count) => { ny.u8(memoryRef).fill(Number(val) & 255, ny.ptr(dst), ny.ptr(dst) + ny.ptr(count)); return 0n; },
      rt_memcpy: (dst, src, count) => { ny.u8(memoryRef).copyWithin(ny.ptr(dst), ny.ptr(src), ny.ptr(src) + ny.ptr(count)); return 0n; },
      rt_load8_idx: (base, idx) => BigInt(ny.u8(memoryRef)[ny.ptr(base) + ny.int(idx)] || 0),
      rt_store8_idx: (base, idx, val) => { ny.u8(memoryRef)[ny.ptr(base) + ny.int(idx)] = Number(val) & 255; return 0n; },
      rt_store64_idx: (base, idx, val) => { ny.writeI64(memoryRef, ny.ptr(base) + ny.int(idx) * 8, val); return 0n; },
      rt_list_new: (n) => ny.list(memoryRef, ny.int(n)),
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
        return r;
      },
      rt_tagof: (v) => ny.tag(Number(ny.tagof(memoryRef, v))),
      rt_is_ok: (v) => ny.bool(ny.tagof(memoryRef, v) === 104n),
      rt_is_err: (v) => ny.bool(ny.tagof(memoryRef, v) === 105n),
      rt_unwrap: (v) => ny.readI64(memoryRef, ny.ptr(v)) || v,
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
        if (BigInt(a || 0) === BigInt(b || 0)) return NY_TRUE;
        if (ny.isFloat(memoryRef, a) || ny.isFloat(memoryRef, b)) return ny.bool(ny.numeric(memoryRef, a) === ny.numeric(memoryRef, b));
        return NY_FALSE;
      },
      rt_has_tag: (v, tag) => {
        const want = BigInt.asIntN(64, BigInt(tag || 0));
        if (want === 110n) return ny.bool(ny.isFloat(memoryRef, v));
        if (want === 121n && ny.tagof(memoryRef, v) === 120n) return NY_TRUE;
        return ny.bool(ny.tagof(memoryRef, v) === want);
      },
      rt_is_float_obj: (v) => ny.bool(ny.isFloat(memoryRef, v)),
      rt_bigint_to_int: (v) => v,
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
      rt_panic: () => { throw new Error("Ny wasm panic"); },
      rt_os_name: () => ny.string(memoryRef, "web"),
      rt_arch_name: () => ny.string(memoryRef, "wasm32"),
      __os_name: () => ny.string(memoryRef, "web"),
      __arch_name: () => ny.string(memoryRef, "wasm32"),
      "std.os.exit": () => 0n,
      "std.os.time.ticks": () => ny.tag(Math.trunc(performance.now() * 1000000)),
      "std.os.time.msleep": () => 0n,
      "std.os.args.first_positive_int": (fallback = 0n) => {
        for (const arg of refreshRunArgv(meta).slice(3)) {
          const n = parseInt(arg, 10);
          if (Number.isFinite(n) && n > 0) return ny.tag(n);
        }
        return fallback;
      },
      "std.os.prim.env": () => ny.string(memoryRef, ""),
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
      "std.core.malloc": (size) => ny.alloc(memoryRef, ny.int(size)),
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
      "std.core.eq": (a, b) => BigInt(a || 0) === BigInt(b || 0) ? NY_TRUE : NY_FALSE,
      "std.core.lt": (a, b) => ny.bool(ny.numeric(memoryRef, a) < ny.numeric(memoryRef, b)),
      "std.core.is_str": (v) => {
        const tag = ny.tagof(memoryRef, v);
        return ny.bool(tag === 120n || tag === 121n);
      },
      "std.core.atoi": (v) => ny.tag(parseInt(ny.text(memoryRef, v, 0), 10) || 0),
      "std.core.to_str": (v) => ny.string(memoryRef, ny.valueToString(memoryRef, v)),
      "std.core.reflect.to_str": (v) => ny.string(memoryRef, ny.valueToString(memoryRef, v)),
      "std.core.reflect.div": (a, b) => ny.tag(Math.trunc(ny.numeric(memoryRef, a) / Math.max(1, ny.numeric(memoryRef, b)))),
      "std.core.reflect.eq": (a, b) => BigInt(a || 0) === BigInt(b || 0) ? NY_TRUE : NY_FALSE,
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
    const host = { ...makeRuntimeImports(meta, memoryRef), ...makeWebImports(meta, memoryRef) };
    const imports = {};
    for (const imp of WebAssembly.Module.imports(module)) {
      if (!imports[imp.module]) imports[imp.module] = {};
      if (imp.kind === "memory") imports[imp.module][imp.name] = memoryRef.memory;
      else if (imp.kind === "table") imports[imp.module][imp.name] = new WebAssembly.Table({ initial: 0, element: "anyfunc" });
      else if (imp.kind === "global") imports[imp.module][imp.name] = new WebAssembly.Global({ value: "i64", mutable: true }, 0n);
      else if (imp.kind === "function") imports[imp.module][imp.name] = host[imp.name] || (() => { throw new Error(`${meta.id}: unsupported import ${imp.module}.${imp.name}`); });
    }
    return imports;
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
      const bytes = meta.wasmBytes || (meta.wasmBase64 ? (() => {
        const raw = atob(meta.wasmBase64);
        const b = new Uint8Array(raw.length);
        for (let i = 0; i < raw.length; i++) b[i] = raw.charCodeAt(i);
        return b;
      })() : await (await fetch(meta.wasm)).arrayBuffer());

      const module = await WebAssembly.compile(bytes);
      const memoryRef = { memory: fallbackMemory, heapTop: 1048576 };
      const instance = await WebAssembly.instantiate(module, makeImports(meta, module, memoryRef));
      if (token !== runtimeToken || currentMeta !== meta) return;
      memoryRef.memory = instance.exports.memory || memoryRef.memory;
      const entry = selectedEntry(instance.exports);
      const browserEntry = ["ny_web_frame", "ny_web_render", "ny_web_main"].some(n => typeof instance.exports[n] === "function");
      const oneShot = !browserEntry;
      currentRuntime = { id: meta.id, exports: instance.exports, memory: memoryRef.memory, entry, ran: false, oneShot };
      setStageMode(oneShot ? "cli" : "web");
      setKernelStatus();
      resetOutput(runtimeHeader(meta, currentRuntime));
      running = autoRunInput && autoRunInput.checked;
      if ($("runBtn")) $("runBtn").textContent = running && !oneShot ? "Pause" : "Run";
      if (typeof instance.exports.ny_web_init === "function") instance.exports.ny_web_init(stage.width, stage.height);
      if (frameTouched && !framePresented) present();
      if (!frameTouched && oneShot) refreshCliStage();
      else if (!frameTouched) drawStatusSurface("Web Ready", [meta.title, `entry: ${entry}`]);
      if (running) setTimeout(() => runFrame(0.016), 0);
    } catch (err) {
      if (token !== runtimeToken || currentMeta !== meta) return;
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
      if (typeof ex.ny_web_frame === "function") ex.ny_web_frame(Number(dt), stage.width, stage.height);
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
      currentRuntime = null;
      setKernelStatus("Error", "warn");
      resetOutput([currentMeta.id, "runtime error", err.message]);
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
    if (running && (!currentRuntime || !currentRuntime.oneShot)) runFrame(dt);
    requestAnimationFrame(loop);
  }

  window.addEventListener("resize", fitCanvas);
  window.addEventListener("hashchange", () => selectDemo(window.location.hash.slice(1), false));
  window.addEventListener("keydown", (e) => { input.key = e.key; input.code = e.keyCode || 0; });
  window.addEventListener("keyup", () => { input.code = 0; });
  canvas.addEventListener("mousemove", (e) => { const r = canvas.getBoundingClientRect(); input.mouse = [e.clientX - r.left, e.clientY - r.top]; });
  canvas.addEventListener("mousedown", () => { input.down = true; });
  canvas.addEventListener("mouseup", () => { input.down = false; });

  wasmFile.addEventListener("change", async () => {
    const file = wasmFile.files[0];
    if (!file) return;
    const meta = { id: "local-" + file.name, title: file.name, source: "local file", area: "LOCAL", mode: "native", wasmBytes: await file.arrayBuffer() };
    selectDemo(meta.id, false);
    demos.push(meta);
    renderList();
    loadRuntime(meta);
  });

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
  selectDemo(window.location.hash.slice(1) || (demos[0] && demos[0].id), false);
  requestAnimationFrame(loop);
})();
