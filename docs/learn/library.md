# Standard library

The facade that owns the problem is the first lookup point. The generated API
page has the exact names and signatures. `ny doc` is the source of truth for
exported functions after the toolchain is built; `docs/spec/` can be read
directly before `ny` exists.

```bash
ny doc search socket
ny doc search --symbols recvuntil
ny doc get std.os.net.remote
```

Use this pattern while exploring:

```bash
ny doc search topic
ny doc search --symbols function_name
ny doc get module.path
```

## Choosing a module

Choose the facade that owns the operation:

| Operation | First module |
| --- | --- |
| Assertions, strings, containers, terminal output | `std.core` |
| Files, paths, time, processes, threads | `std.os` |
| HTTP, sockets, process tubes, transcripts | `std.os.net` |
| Windows, input, drawing, textures, scenes | `std.os.ui.render` and `std.os.ui.window` |
| JSON, YAML, TOML, CSV, XML, SQL | `std.parse.data` |
| Images, glTF, meshes | `std.parse.img` and `std.parse.3d` |
| Number theory, algebra, matrices, SIMD | `std.math` |
| Encodings, hashes, ciphers, public-key helpers, analysis | `std.math.crypto` |

Once you know the facade, use `ny doc get` on the module and import it with an
alias if the call sites would otherwise be unclear.

## Domains

| Domain | Facade | Scope |
| --- | --- | --- |
| Core runtime | `std.core` | primitives, containers, strings, assertions, reflection, terminal APIs, queues, channels |
| Operating system | `std.os` | files, paths, processes, environment, time, threads, async, platform features |
| Networking | `std.os.net` | HTTP clients, HTTP servers, sockets, TLS transport, tubes, transcripts |
| UI and rendering | `std.os.ui`, `std.os.ui.window`, `std.os.ui.render` | windows, frame loops, input, drawing, text, textures, meshes, snapshots |
| Data formats | `std.parse.data` | JSON, YAML, TOML, CSV, XML, SQL, zlib |
| Syntax parsers | `std.parse.syntax` | tokenizers for Nytrix and common source formats |
| Images and assets | `std.parse.img`, `std.parse.3d` | image codecs, 3D asset loading, glTF workflows |
| Math | `std.math` | scalar math, big integers, vectors, matrices, rings, polynomials, number theory, SIMD |
| Crypto and analysis | `std.math.crypto` | encodings, hashes, symmetric ciphers, public-key helpers, number theory, PRNGs, analysis |

## Core runtime

| Module | Scope |
| --- | --- |
| `std.core` | Common runtime facade. |
| `std.core.str` | ASCII, UTF-8, split/join, hex, builders, string conversion. |
| `std.core.iter` | Iterators and sequence transforms. |
| `std.core.collections` | Counters, queues, channels, collection operations. |
| `std.core.reflect` | Shape checks, equality, representation, generic access. |
| `std.core.syntax` | Syntax registry, macro/attribute handlers, type-group helpers, rewrites. |
| `std.core.error` | Panic, warning, error, and `Result` operations. |
| `std.core.term` | ANSI color, tables, progress, canvas, TUI output. |
| `std.core.test` | Assertions and unit-test support. |
| `std.core.tbuf` | Raw typed buffers. |

### `std.core.syntax`

Exports the explicit syntax-extension and type-helper surface used by runtime
tests. Main entry points include `new_registry`, `registry`,
`reset_registry`, `register_macro`, `register_macro_in`,
`register_attribute`, `register_attribute_in`, `form`, `form_head`,
`form_tail`, `expand_macro`, `expand_form`, `expand_form_deep`,
`new_rewriter`, `register_rewrite`, `rewrite_once`, and
`rewrite_fixpoint`.

Type helpers live under `std.core.syntax.type`: `normalize_type_name`,
`is_type`, `require_type`, `assert_type`, `define_type_alias`,
`define_type_group`, and `extend_type_group`.

## Operating system

| Module | Scope |
| --- | --- |
| `std.os` | Common OS facade. |
| `std.os.args` | Program arguments and flag lookup. |
| `std.os.path` | Separators, joins, normalization, basename/dirname, user/cache/config paths. |
| `std.os.fs` | File and directory queries, directory listing, walking. |
| `std.os.io` | Process IO, pipes, subprocess streams. |
| `std.os.process` | Process spawning and command execution. |
| `std.os.time` | Wall-clock time, monotonic timers, sleeps, formatting. |
| `std.os.thread` | Threads, mutexes, coordination. |
| `std.os.parallel` | Futures, worker-thread maps, and scheduler policy helpers. |
| `std.os.async` | Stackless async tasks and scheduling. |
| `std.os.ffi` | Native library loading and FFI boundaries. |
| `std.os.disasm` | Assembly, disassembly, hexdump, shellcode utilities. |

## OS module surfaces

These modules export flat function names. Import aliases namespace the exported
names; they do not create object-style methods that are not exported.

### `std.os.time`

Exports: `time`, `now`, `unix`, `now_ms`, `sleep`, `msleep`, `ticks`,
`monotonic_ns`, `Instant`, `instant`, `since_ns`, `since_ms`, `Timer`,
`timer`, `timer_start`, `elapsed_ns`, `elapsed_ms`, `elapsed_sec`, `reset`,
`format`, `format_time`.

Use `msleep(ms)` for millisecond sleeps. `sleep_ms` is not exported. Use
`now_ms()` for Unix wall-clock milliseconds and `ticks()` or `monotonic_ns()`
for elapsed-time measurements.

### `std.os.thread`

Exports: `thread_spawn`, `thread_spawn_call`, `thread_launch`,
`thread_launch_call`, `thread_join`, `mutex_new`, `mutex_lock`,
`mutex_unlock`, `mutex_free`.

The API is flat:

```ny
use std.core
use std.os.thread

fn worker(any: arg): any { arg }

def handle = thread_spawn(worker, "ok")
assert_eq(thread_join(handle), "ok", "thread")
```

`thread.spawn` is not exported.

### `std.os.parallel`

Exports: `parallel_mode`, `parallel_threads`, `parallel_min_work`,
`parallel_should_threads`, `parallel_status`, `hardware_threads`,
`thread_budget`, `future`, `async`, `detach`, `future_wait`, `parallel_map`,
`parallel_map_indexed`, `parallel_each`, `chunk_ranges`, `scheduler_policy`,
`scheduler_status`, `work_stealing_enabled`, `work_stealing_plan`,
`work_queue`, `work_queue_push`, `work_queue_pop`, `work_queue_steal`.

Use `future(work, arg)` or `async(work, arg)` for a joinable worker handle, then
`future_wait(handle)`. Use `detach(work, arg)` for fire-and-forget work.
`fork`, `task`, and `join` are not exported names in this module.

`parallel_map(xs, f)` and `parallel_map_indexed(xs, f)` return result lists in
input order. `parallel_each(xs, f)` returns the number of processed items and
does not collect worker return values.

## Networking

| Module | Scope |
| --- | --- |
| `std.os.net` | Network facade and shared context. |
| `std.os.net.requests` | Requests-style HTTP options and response dictionaries. |
| `std.os.net.server` | Blocking HTTP/1.1 servers. |
| `std.os.net.remote` | Tube-style process, socket, SSH, and transcript APIs. |
| `std.os.net.socket` | Direct TCP sockets. |
| `std.os.net.http` | HTTP parsing and response construction. |
| `std.os.net.curl` | Curl-backed HTTPS transport when available. |

## UI and rendering

| Module | Scope |
| --- | --- |
| `std.os.ui` | UI facade for windowing, rendering, camera, scene, assets, profiling, and probes. |
| `std.os.ui.window` | Window creation, lifecycle, monitors, keyboard, mouse, gamepads, events, clipboard, backend hooks. |
| `std.os.ui.consts` | Key codes, event IDs, window flags, modifier masks. |
| `std.os.ui.window.input` | Mouse-button names, key notation parsing, chord checks. |
| `std.os.ui.render` | Frame loop, draw calls, colors, fonts, textures, cameras, meshes, capture, renderer stats. |
| `std.os.ui.render.vk` | Vulkan backend operations and lower-level renderer controls. |
| `std.os.ui.app` | Application lifecycle setup for larger UI programs. |
| `std.os.ui.camera` | Camera state and movement. |
| `std.os.ui.assets` | Asset catalog and browser support. |

Use [ui.md](ui.md) for a first window, drawing, input, textures, and 3D start.

## Parsing and formats

| Module | Scope |
| --- | --- |
| `std.parse.data.json` | JSON decode/encode and parse errors. |
| `std.parse.data.yaml` | YAML decode/encode. |
| `std.parse.data.toml` | TOML decode/encode. |
| `std.parse.data.csv` | CSV decode/encode. |
| `std.parse.data.xml` | XML node construction, parse, encode. |
| `std.parse.data.sql` | SQL tokenizer, parser, normalization, statement kind. |
| `std.parse.data.zlib` | zlib/deflate compression. |
| `std.parse.syntax` | Tokenizer facade for source formats. |
| `std.parse.img` | Image load/save/decode/encode facade. |
| `std.parse.3d.gltf` | glTF loading, scene inspection, meshes, materials. |

3D asset references:

- [Khronos glTF](https://github.com/KhronosGroup/glTF): glTF specification and extension registry entry point.
- [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets): conformance and feature sample assets.
- [Needle Asset Explorer](https://asset-explorer.needle.tools/): browser asset inspection and debugging.

## Math

| Module | Scope |
| --- | --- |
| `std.math` | Numeric facade. |
| `std.math.integer` | Integer arithmetic. |
| `std.math.float` | Floating-point operations. |
| `std.math.big` | Arbitrary-precision integer and fixed-point operations. |
| `std.math.bin` | Endian reads/writes, bit operations, packing, padding, byte codecs. |
| `std.math.vector` | Vector operations. |
| `std.math.matrix` | Matrix operations. |
| `std.math.ring` | Rings, finite fields, polynomial elements. |
| `std.math.nt` | Number theory. |
| `std.math.random` | Random number generation. |
| `std.math.simmd` | SIMD and instruction-level operations. |

## Cryptography and analysis

| Module | Scope |
| --- | --- |
| `std.math.crypto` | Facade for crypto primitives, encodings, number theory, and analysis helpers. |
| `std.math.crypto.encoding` | ASCII, bytes, XOR, radix/base conversion, PEM, DER, ASN.1. |
| `std.math.crypto.hash` | Hash, HMAC, password-hash, NTLM, and length-extension helpers. |
| `std.math.crypto.symmetric` | AES, DES, TEA, Salsa20, ChaCha20, S-box analysis. |
| `std.math.crypto.block.mode` | CBC, ECB, GCM, IGE, OCB2, padding, block utilities. |
| `std.math.crypto.block.stream` | OTP, RC4, CTR helpers, keystream scoring. |
| `std.math.crypto.cipher` | Classical ciphers, scoring, and text-analysis helpers. |
| `std.math.crypto.rsa` | RSA keys, operations, signatures, PKCS#1 v1.5, modular arithmetic helpers. |
| `std.math.crypto.ecc` | Curve arithmetic, ECDSA, Edwards/Montgomery forms, ElGamal helpers. |
| `std.math.crypto.lattice` | LLL, CVP, BKZ, basis matrices, reduction reports. |
| `std.math.crypto.factorization` | Fermat, Pollard, ECM, primality, known-prime helpers. |
| `std.math.crypto.prng` | LCG, LFSR, MT19937, PCG, xoshiro helpers. |
| `std.math.crypto.analysis` | Quadgrams, frequency tables, dictionaries, side-channel checks. |

## API reference

The generated reference owns signatures and docstrings. The table above maps
domain choice before `ny doc search` or `ny doc get`.

Related guides:

- [networking.md](networking.md) for HTTP, sockets, tubes, and TLS.
- [native.md](native.md) for FFI and native resources.
- [metaprogramming.md](metaprogramming.md) for compile-time generation.
