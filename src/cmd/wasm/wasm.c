#include "wasm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char wasm_runner_js[] =
  "const fs = require('fs');\n"
  "const wasmPath = process.argv[2];\n"
  "const extraArgs = process.argv.slice(3);\n"
  "if (!wasmPath) { console.error('Usage: ny wasm <module.wasm> [args...]'); process.exit(1); }\n"
  "const wasmBin = fs.readFileSync(wasmPath);\n"
  "\n"
  "const PTR = 0n, INT = 1n, TAG = 2n, BOOL = 3n, FLT = 4n, TEXT = 5n;\n"
  "const TAG_BITS = 7n, SHIFT = 3n;\n"
  "const NY_TRUE = 8n, NY_FALSE = 2n;\n"
  "\n"
  "const ofVal = (v, t) => (v << SHIFT) | t;\n"
  "const ptr = (v) => ofVal(BigInt(v), PTR);\n"
  "const inte = (v) => ofVal(BigInt(v), INT);\n"
  "const boolV = (v) => v ? NY_TRUE : NY_FALSE;\n"
  "const tagof = (v) => v & TAG_BITS;\n"
  "const dataV = (v) => v >> SHIFT;\n"
  "\n"
  "let bump = 0;\n"
  "function alloc(n, mem) {\n"
  "  let aligned = (n + 7) & ~7;\n"
  "  let addr = bump;\n"
  "  bump += aligned;\n"
  "  while (bump > mem.buffer.byteLength) mem.grow(1);\n"
  "  new Uint8Array(mem.buffer, addr, aligned).fill(0);\n"
  "  return addr;\n"
  "}\n"
  "\n"
  "function textEncode(str, mem) {\n"
  "  let enc = new TextEncoder().encode(str);\n"
  "  let hdr = 8;\n"
  "  let addr = alloc(enc.length + hdr, mem);\n"
  "  let dv = new DataView(mem.buffer);\n"
  "  dv.setUint32(addr, enc.length, true);\n"
  "  new Uint8Array(mem.buffer, addr + hdr, enc.length).set(enc);\n"
  "  return ofVal(BigInt(addr), TEXT);\n"
  "}\n"
  "function textDecode(v, mem) {\n"
  "  if (tagof(v) === TEXT) {\n"
  "    let a = Number(dataV(v));\n"
  "    let dv = new DataView(mem.buffer);\n"
  "    let len = dv.getUint32(a, true);\n"
  "    let bytes = new Uint8Array(mem.buffer, a + 8, len);\n"
  "    return new TextDecoder().decode(bytes);\n"
  "  }\n"
  "  if (tagof(v) === INT) return String(dataV(v));\n"
  "  return '';\n"
  "}\n"
  "\n"
  "function listNew(mem) {\n"
  "  let cap = 4;\n"
  "  let addr = alloc(8 + cap * 8, mem);\n"
  "  let dv = new DataView(mem.buffer);\n"
  "  dv.setUint32(addr, 0, true);\n"
  "  dv.setUint32(addr + 4, cap, true);\n"
  "  return ofVal(BigInt(addr), PTR);\n"
  "}\n"
  "function listAppend(l, val, mem) {\n"
  "  let a = Number(dataV(l));\n"
  "  let dv = new DataView(mem.buffer);\n"
  "  let len = dv.getUint32(a, true);\n"
  "  let cap = dv.getUint32(a + 4, true);\n"
  "  if (len >= cap) {\n"
  "    let newCap = cap * 2 || 4;\n"
  "    let newA = alloc(8 + newCap * 8, mem);\n"
  "    let old = new Uint8Array(mem.buffer, a, 8 + cap * 8);\n"
  "    new Uint8Array(mem.buffer, newA, old.length).set(old);\n"
  "    a = newA;\n"
  "    dv = new DataView(mem.buffer);\n"
  "    dv.setUint32(a + 4, newCap, true);\n"
  "  }\n"
  "  dv.setBigUint64(a + 8 + len * 8, val, true);\n"
  "  dv.setUint32(a, len + 1, true);\n"
  "}\n"
  "\n"
  "function fltBox(v) {\n"
  "  let buf = new ArrayBuffer(8);\n"
  "  new Float64Array(buf)[0] = v;\n"
  "  return ofVal(new BigUint64Array(buf)[0], FLT);\n"
  "}\n"
  "function fltUnbox(v) {\n"
  "  let buf = new ArrayBuffer(8);\n"
  "  new BigUint64Array(buf)[0] = dataV(v);\n"
  "  return new Float64Array(buf)[0];\n"
  "}\n"
  "\n"
  "let M = null;\n"
  "let _printBuf = '';\n"
  "\n"
  "function getDV() { return new DataView(M.buffer); }\n"
  "\n"
  "const env = {\n"
  "  rt_malloc: (n) => ptr(alloc(Number(dataV(n)), M)),\n"
  "  rt_free: () => 0n,\n"
  "  rt_realloc: (p, n) => ptr(alloc(Number(dataV(n)), M)),\n"
  "  rt_try_gc: () => 0n,\n"
  "\n"
  "  rt_set_len: (s, n) => { let dv = getDV(); dv.setUint32(Number(dataV(s)), Number(dataV(n)), true); return s; },\n"
  "  rt_len: (s) => {\n"
  "    let t = tagof(s);\n"
  "    if (t === INT) return s;\n"
  "    if (t === TEXT || t === PTR) {\n"
  "      let dv = getDV();\n"
  "      return inte(dv.getUint32(Number(dataV(s)), true));\n"
  "    }\n"
  "    return 0n;\n"
  "  },\n"
  "\n"
  "  rt_concat: (a, b) => textEncode(textDecode(a, M) + textDecode(b, M), M),\n"
  "  rt_crash: (msg) => { console.error('crash:', textDecode(msg, M)); process.exit(1); return 0n; },\n"
  "  rt_panic: (msg) => { console.error('panic:', textDecode(msg, M)); process.exit(1); return 0n; },\n"
  "  rt_die: (msg) => { console.error('error:', textDecode(msg, M)); process.exit(1); return 0n; },\n"
  "  rt_assert: (cond, msg) => { if (cond === NY_FALSE) { console.error('assert:', textDecode(msg, M)); process.exit(1); } return 0n; },\n"
  "\n"
  "  rt_range: (start, end, step) => {\n"
  "    let s = Number(dataV(start)), e = Number(dataV(end)), st = Number(dataV(step));\n"
  "    let l = listNew(M);\n"
  "    for (let i = s; i < e; i += st) listAppend(l, inte(i), M);\n"
  "    return l;\n"
  "  },\n"
  "\n"
  "  rt_putchar: (c) => { process.stdout.write(String.fromCodePoint(Number(dataV(c)))); return c; },\n"
  "  rt_getchar: () => {\n"
  "    try { let b = Buffer.alloc(1); let n = fs.readSync(0, b, 0, 1); return n ? inte(b[0]) : inte(-1); }\n"
  "    catch (e) { return inte(-1); }\n"
  "  },\n"
  "\n"
  "  rt_print_int: (v) => { _printBuf += String(dataV(v)); return v; },\n"
  "  rt_print_float: (v) => { _printBuf += String(fltUnbox(v)); return v; },\n"
  "  rt_print_char: (v) => { _printBuf += String.fromCodePoint(Number(dataV(v))); return v; },\n"
  "  rt_print: (v) => { _printBuf += textDecode(v, M); return v; },\n"
  "  rt_print_list: (v) => {\n"
  "    _printBuf += '[';\n"
  "    let a = Number(dataV(v));\n"
  "    let dv = getDV();\n"
  "    let len = dv.getUint32(a, true);\n"
  "    for (let i = 0; i < len; i++) {\n"
  "      if (i) _printBuf += ', ';\n"
  "      dv = getDV();\n"
  "      _printBuf += textDecode(dv.getBigUint64(a + 8 + i * 8, true), M);\n"
  "    }\n"
  "    _printBuf += ']';\n"
  "    return v;\n"
  "  },\n"
  "  rt_print_str: (v) => { _printBuf += textDecode(v, M); return v; },\n"
  "  rt_print_newline: () => { _printBuf += '\\n'; return 0n; },\n"
  "  rt_flush: () => { process.stdout.write(_printBuf); _printBuf = ''; return 0n; },\n"
  "\n"
  "  rt_int_str: (v) => textEncode(String(dataV(v)), M),\n"
  "  rt_append: (l, v) => { listAppend(l, v, M); return l; },\n"
  "\n"
  "  rt_list_new: () => listNew(M),\n"
  "  rt_load_item: (l, i) => { let dv = getDV(); return dv.getBigUint64(Number(dataV(l)) + 8 + Number(dataV(i)) * 8, true); },\n"
  "  rt_store_item: (l, i, val) => { let dv = getDV(); dv.setBigUint64(Number(dataV(l)) + 8 + Number(dataV(i)) * 8, val, true); return 0n; },\n"
  "  rt_list_len: (l) => { let dv = getDV(); return inte(dv.getUint32(Number(dataV(l)), true)); },\n"
  "\n"
  "  rt_exit: (code) => { process.stdout.write(_printBuf); process.exit(Number(dataV(code))); return 0n; },\n"
  "\n"
  "  rt_open: (path, flags, mode) => {\n"
  "    let p = textDecode(path, M), f = Number(dataV(flags)), m = Number(dataV(mode));\n"
  "    let sf = f === 0 ? fs.constants.O_RDONLY : f === 1 ? fs.constants.O_WRONLY|fs.constants.O_CREAT|fs.constants.O_TRUNC : fs.constants.O_RDWR|fs.constants.O_CREAT;\n"
  "    try { return inte(fs.openSync(p, sf, m)); } catch(e) { return inte(-1); }\n"
  "  },\n"
  "  rt_close: (fd) => { try { fs.closeSync(Number(dataV(fd))); } catch(e){} return 0n; },\n"
  "  rt_read_off: (fd, buf, n, off) => {\n"
  "    let f = Number(dataV(fd)), a = Number(dataV(buf)) + Number(dataV(off)), len = Number(dataV(n));\n"
  "    try { let b = Buffer.alloc(len); let r = fs.readSync(f, b, 0, len); if (r > 0) new Uint8Array(M.buffer, a, r).set(b.subarray(0, r)); return inte(r); }\n"
  "    catch(e) { return inte(-1); }\n"
  "  },\n"
  "  rt_write_off: (fd, buf, n, off) => {\n"
  "    let f = Number(dataV(fd)), a = Number(dataV(buf)) + Number(dataV(off)), len = Number(dataV(n));\n"
  "    try { let b = new Uint8Array(M.buffer, a, len); return inte(fs.writeSync(f, b)); }\n"
  "    catch(e) { return inte(-1); }\n"
  "  },\n"
  "  rt_write: (fd, buf, n, off) => {\n"
  "    let f = Number(dataV(fd)), a = Number(dataV(buf)) + Number(dataV(off)), len = Number(dataV(n));\n"
  "    try { let b = new Uint8Array(M.buffer, a, len); return inte(fs.writeSync(f, b)); }\n"
  "    catch(e) { return inte(-1); }\n"
  "  },\n"
  "\n"
  "  rt_load8_idx: (p, i) => { let dv = getDV(); return inte(dv.getUint8(Number(dataV(p)) + Number(dataV(i)))); },\n"
  "  rt_load16_idx: (p, i) => { let dv = getDV(); return inte(dv.getUint16(Number(dataV(p)) + Number(dataV(i)), true)); },\n"
  "  rt_load32_idx: (p, i) => { let dv = getDV(); return inte(dv.getUint32(Number(dataV(p)) + Number(dataV(i)), true)); },\n"
  "  rt_load64_idx: (p, i) => { let dv = getDV(); return dv.getBigUint64(Number(dataV(p)) + Number(dataV(i)), true); },\n"
  "  rt_store8_idx: (p, i, v) => { let dv = getDV(); dv.setUint8(Number(dataV(p)) + Number(dataV(i)), Number(dataV(v))); return 0n; },\n"
  "  rt_store16_idx: (p, i, v) => { let dv = getDV(); dv.setUint16(Number(dataV(p)) + Number(dataV(i)), Number(dataV(v)), true); return 0n; },\n"
  "  rt_store32_idx: (p, i, v) => { let dv = getDV(); dv.setUint32(Number(dataV(p)) + Number(dataV(i)), Number(dataV(v)), true); return 0n; },\n"
  "  rt_store64_idx: (p, i, v) => { let dv = getDV(); dv.setBigUint64(Number(dataV(p)) + Number(dataV(i)), v, true); return 0n; },\n"
  "\n"
  "  rt_time_milliseconds: () => inte(Date.now()),\n"
  "  rt_ticks_ns: () => inte(BigInt(Math.floor(performance.now() * 1e6))),\n"
  "  rt_getpid: () => inte(process.pid),\n"
  "  rt_time_seconds: () => inte(Math.floor(Date.now() / 1000)),\n"
  "  rt_msleep_ms: (ms) => { Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, Number(dataV(ms))); return 0n; },\n"
  "  rt_getcwd: (buf, size) => {\n"
  "    try {\n"
  "      let cwd = process.cwd();\n"
  "      let b = Buffer.from(cwd);\n"
  "      let max = Number(dataV(size));\n"
  "      let len = Math.min(b.length, max - 1);\n"
  "      new Uint8Array(M.buffer, Number(dataV(buf)), len).set(b.subarray(0, len));\n"
  "      new Uint8Array(M.buffer, Number(dataV(buf)) + len, 1)[0] = 0;\n"
  "      return inte(len);\n"
  "    } catch(e) { return inte(-1); }\n"
  "  },\n"
  "\n"
  "  rt_flt_box_val: (v) => fltBox(Number(dataV(v))),\n"
  "  rt_flt_unbox_val: (v) => Number(dataV(v)),\n"
  "  rt_flt_box_val32: (v) => fltBox(Number(dataV(v))),\n"
  "  rt_flt_unbox_val32: (v) => Number(dataV(v)),\n"
  "  rt_flt_add: (a, b) => fltBox(fltUnbox(a) + fltUnbox(b)),\n"
  "  rt_flt_sub: (a, b) => fltBox(fltUnbox(a) - fltUnbox(b)),\n"
  "  rt_flt_mul: (a, b) => fltBox(fltUnbox(a) * fltUnbox(b)),\n"
  "  rt_flt_div: (a, b) => fltBox(fltUnbox(a) / fltUnbox(b)),\n"
  "  rt_flt_lt: (a, b) => boolV(fltUnbox(a) < fltUnbox(b)),\n"
  "  rt_flt_gt: (a, b) => boolV(fltUnbox(a) > fltUnbox(b)),\n"
  "  rt_flt_eq: (a, b) => boolV(fltUnbox(a) === fltUnbox(b)),\n"
  "  rt_flt_is_nan: (v) => boolV(isNaN(fltUnbox(v))),\n"
  "  rt_flt_is_inf: (v) => boolV(!isFinite(fltUnbox(v))),\n"
  "  rt_flt_from_int: (v) => fltBox(Number(dataV(v))),\n"
  "  rt_flt_to_int: (v) => inte(Math.floor(fltUnbox(v))),\n"
  "  rt_flt_sqrt: (v) => fltBox(Math.sqrt(fltUnbox(v))),\n"
  "  rt_flt_floor: (v) => fltBox(Math.floor(fltUnbox(v))),\n"
  "  rt_flt_ceil: (v) => fltBox(Math.ceil(fltUnbox(v))),\n"
  "  rt_flt_round: (v) => fltBox(Math.round(fltUnbox(v))),\n"
  "  rt_flt_pow: (a, b) => fltBox(Math.pow(fltUnbox(a), fltUnbox(b))),\n"
  "  rt_flt_fmod: (a, b) => fltBox(fltUnbox(a) % fltUnbox(b)),\n"
  "  rt_flt_trunc: (v) => fltBox(Math.trunc(fltUnbox(v))),\n"
  "  rt_flt_exp: (v) => fltBox(Math.exp(fltUnbox(v))),\n"
  "  rt_flt_log: (v) => fltBox(Math.log(fltUnbox(v))),\n"
  "  rt_flt_log2: (v) => fltBox(Math.log2(fltUnbox(v))),\n"
  "  rt_flt_log10: (v) => fltBox(Math.log10(fltUnbox(v))),\n"
  "  rt_flt_sin: (v) => fltBox(Math.sin(fltUnbox(v))),\n"
  "  rt_flt_cos: (v) => fltBox(Math.cos(fltUnbox(v))),\n"
  "  rt_flt_tan: (v) => fltBox(Math.tan(fltUnbox(v))),\n"
  "  rt_flt_asin: (v) => fltBox(Math.asin(fltUnbox(v))),\n"
  "  rt_flt_acos: (v) => fltBox(Math.acos(fltUnbox(v))),\n"
  "  rt_flt_atan: (v) => fltBox(Math.atan(fltUnbox(v))),\n"
  "  rt_flt_atan2: (a, b) => fltBox(Math.atan2(fltUnbox(a), fltUnbox(b))),\n"
  "  rt_flt_nan: () => fltBox(NaN),\n"
  "  rt_flt_inf: () => fltBox(Infinity),\n"
  "  rt_flt_hash: (v) => inte(Number(dataV(v) & 0x7FFFFFFFFFFFFFFFn)),\n"
  "\n"
  "  rt_memcpy: (dst, src, n) => {\n"
  "    let d = Number(dataV(dst)), s = Number(dataV(src)), len = Number(dataV(n));\n"
  "    new Uint8Array(M.buffer, d, len).set(new Uint8Array(M.buffer, s, len));\n"
  "    return 0n;\n"
  "  },\n"
  "  rt_memset: (dst, v, n) => {\n"
  "    let d = Number(dataV(dst)), val = Number(dataV(v)), len = Number(dataV(n));\n"
  "    new Uint8Array(M.buffer, d, len).fill(val);\n"
  "    return 0n;\n"
  "  },\n"
  "  rt_memcmp: (a, b, n) => {\n"
  "    let a1 = Number(dataV(a)), a2 = Number(dataV(b)), len = Number(dataV(n));\n"
  "    let ba = new Uint8Array(M.buffer, a1, len);\n"
  "    let bb = new Uint8Array(M.buffer, a2, len);\n"
  "    for (let i = 0; i < len; i++) { if (ba[i] !== bb[i]) return inte(ba[i] - bb[i]); }\n"
  "    return 0n;\n"
  "  },\n"
  "  rt_getenv: (name) => {\n"
  "    let n = textDecode(name, M);\n"
  "    let v = process.env[n];\n"
  "    return v !== undefined ? textEncode(v, M) : NY_FALSE;\n"
  "  },\n"
  "};\n"
  "\n"
  "(async () => {\n"
  "  try {\n"
  "    let mod = new WebAssembly.Module(wasmBin);\n"
  "    let instance = await WebAssembly.instantiate(mod, { env });\n"
  "    M = instance.exports.memory;\n"
  "    if (!M) { console.error('ny-wasm: module has no exported memory'); process.exit(1); }\n"
  "\n"
  "    let mainFn = instance.exports.main || instance.exports._start;\n"
  "    if (!mainFn) { console.error('ny-wasm: no main/_start export found'); process.exit(1); }\n"
  "\n"
  "    let argsList = listNew(M);\n"
  "    for (let a of extraArgs) listAppend(argsList, textEncode(a, M), M);\n"
  "\n"
  "    let result = mainFn(inte(extraArgs.length), argsList);\n"
  "    process.stdout.write(_printBuf);\n"
  "    process.exit(Number(dataV(result)) || 0);\n"
  "  } catch (e) {\n"
  "    console.error('ny-wasm:', e.message || e);\n"
  "    process.exit(1);\n"
  "  }\n"
  "})();\n"
  ;

static int write_runner(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f)
    return -1;
  size_t len = strlen(wasm_runner_js);
  size_t written = fwrite(wasm_runner_js, 1, len, f);
  int flush_ok = fclose(f) == 0;
  return (written == len && flush_ok) ? 0 : -1;
}

static void cleanup_run(const char *runner_path, const char *tmpdir,
                        char **child_argv) {
  if (runner_path && *runner_path)
    unlink(runner_path);
  if (tmpdir && *tmpdir)
    rmdir(tmpdir);
  free(child_argv);
}

static char **build_node_argv(const char *runner_path, int argc, char **argv) {
  int total = 3 + argc;
  char **child_argv = (char **)calloc((size_t)total, sizeof(char *));
  if (!child_argv)
    return NULL;
  int idx = 0;
  child_argv[idx++] = "node";
  child_argv[idx++] = (char *)runner_path;
  for (int i = 1; i < argc; i++)
    child_argv[idx++] = argv[i];
  child_argv[idx] = NULL;
  return child_argv;
}

static int wait_child_exit_status(pid_t pid) {
  int status = 0;
  for (;;) {
    pid_t r = waitpid(pid, &status, 0);
    if (r >= 0)
      break;
    if (errno != EINTR) {
      perror("waitpid");
      return -1;
    }
  }
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) {
    fprintf(stderr, "ny-wasm: node terminated by signal %d\n", WTERMSIG(status));
    return 1;
  }
  return 0;
}

int ny_wasm_main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: ny wasm <module.wasm> [args...]\n");
    return 1;
  }

  char tmpdir[] = "/tmp/ny-wasm-XXXXXX";
  if (!mkdtemp(tmpdir)) {
    perror("mkdtemp");
    return 1;
  }

  char runner_path[4096];
  snprintf(runner_path, sizeof(runner_path), "%s/runner.js", tmpdir);
  if (write_runner(runner_path)) {
    fprintf(stderr, "ny-wasm: failed to write runner\n");
    cleanup_run(runner_path, tmpdir, NULL);
    return 1;
  }

  char **child_argv = build_node_argv(runner_path, argc, argv);
  if (!child_argv) {
    fprintf(stderr, "ny-wasm: oom\n");
    cleanup_run(runner_path, tmpdir, NULL);
    return 1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    cleanup_run(runner_path, tmpdir, child_argv);
    return 1;
  }

  if (pid == 0) {
    execvp("node", child_argv);
    fprintf(stderr, "ny-wasm: node not found (install Node.js or check PATH)\n");
    _exit(127);
  }

  int rc = wait_child_exit_status(pid);
  cleanup_run(runner_path, tmpdir, child_argv);
  return rc < 0 ? 1 : rc;
}
