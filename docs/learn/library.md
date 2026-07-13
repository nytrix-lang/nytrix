# Standard Library

Start with the facade that owns the problem, then ask generated docs for exact
signatures and exported names.

```bash
ny doc search socket
ny doc search --symbols recvuntil
ny doc get std.os.net.remote
```

## First Module

| Need | First lookup |
| --- | --- |
| Assertions, strings, containers, terminal output | `std.core` |
| Files, paths, time, processes, threads | `std.os` |
| HTTP, sockets, process tubes, transcripts | `std.os.net` |
| Windows, drawing, input, gamepads, textures, scenes | `std.os.ui.render`, `std.os.ui.window` |
| JSON, YAML, TOML, CSV, XML, SQL, zlib | `std.math.parse.data` |
| Images, fonts, glTF, meshes | `std.math.parse.img`, `std.math.parse.font.truetype`, `std.math.parse.3d.gltf` |
| Scalars, big ints, matrices, number theory, SIMD | `std.math` |
| Encodings, hashes, ciphers, public-key helpers, analysis | `std.math.crypto` |

Use aliases when call sites would otherwise hide ownership:

```ny
use std.math.parse.data.json as json
def obj = json.json_decode("{\"ok\":true}")
```

## Domains

| Domain | Modules |
| --- | --- |
| Core | `std.core`, `std.core.str`, `std.core.iter`, `std.core.collections`, `std.core.dict`, `std.core.set`, `std.core.tuple`, `std.core.counter`, `std.core.reflect`, `std.core.regex`, `std.core.error`, `std.core.term`, `std.core.test`, `std.core.tbuf`, `std.core.mem`, `std.core.io`, `std.core.glob`, `std.core.progress`, `std.core.inspect` |
| Syntax helpers | `std.core.syntax`, `std.core.syntax.builtin`, `std.core.syntax.type` |
| OS | `std.os`, `std.os.args`, `std.os.path`, `std.os.fs`, `std.os.io`, `std.os.process`, `std.os.subprocess`, `std.os.time`, `std.os.clock`, `std.os.thread`, `std.os.atomic`, `std.os.parallel`, `std.os.async`, `std.os.ffi`, `std.os.disasm`, `std.os.gpu`, `std.os.accel`, `std.os.sound`, `std.os.clipboard`, `std.os.info`, `std.os.platform`, `std.os.sys` |
| Networking | `std.os.net`, `std.os.net.requests`, `std.os.net.server`, `std.os.net.remote`, `std.os.net.socket`, `std.os.net.http`, `std.os.net.curl` |
| UI | `std.os.ui`, `std.os.ui.window`, `std.os.ui.window.consts`, `std.os.ui.window.input`, `std.os.ui.render` (backend facade), `std.os.ui.render.gl`, `std.os.ui.render.vk`, `std.os.ui.render.viewer`, `std.os.ui.render.viewer.widgets`, `std.os.ui.render.viewer.input`, `std.os.ui.render.viewer.window`, `std.os.ui.render.viewer.clipboard`, `std.os.ui.render.viewer.app`, `std.os.ui.render.camera`, `std.os.ui.assets` |
| Data | `std.math.parse.data.json`, `yaml`, `toml`, `csv`, `xml`, `sql`, `zlib` |
| Syntax parsers | `std.math.parse.syntax.nytrix`, `c`, `javascript`, `typescript`, `python`, `bash`, `lua`, `html`, `markdown`, `json`, `xml`, `yaml`, `cmake`, `assembly` |
| Assets | `std.math.parse.img`, `std.math.parse.img.png`, `jpeg`, `gif`, `bmp`, `svg`, `tga`, `webp`, `exr`, `std.math.parse.font.truetype`, `std.math.parse.3d.gltf`, `meshopt`, `obj` |
| Math | `std.math`, `integer`, `float`, `scalar`, `big`, `bigrat`, `bin`, `complex`, `ct`, `gf`, `hensel`, `logic`, `matrix`, `noise`, `nt`, `ntt`, `poly`, `quat`, `random`, `ring`, `simmd`, `smt`, `stat`, `vector` |
| Crypto | `std.math.crypto.encoding`, `hash`, `symmetric`, `block.mode`, `block.stream`, `cipher`, `rsa`, `ecc`, `lattice`, `factorization`, `prng`, `analysis` |

`std.math.logic` provides self-hosted propositional reasoning. Propositions are
ordinary explicit dictionaries created with `prop_true`, `prop_false`,
`prop_atom`, `prop_not`, `prop_and`, `prop_or`, `prop_implies`, and `prop_iff`.
Use `prop_is` to validate external data, `prop_eval` with an atom environment,
and `prop_simplify` for deterministic constant simplification.

`prop_tautology_report` exhaustively decides bounded propositions and returns
`decided`, `valid`, the variable list, a counterexample assignment, and the
checked/required assignment counts. Its
default limit is 16 variables and the accepted maximum is 20; exceeding the
chosen limit returns `decided=false` instead of silently guessing or running
without a bound. `prop_tautology` is the compact boolean facade.

The short vocabulary keeps ordinary code readable:

```ny
use std.math.logic as logic

def p = logic.atom("p")
def identity = logic.implies(logic.conj(p, logic.truth()), p)
def result = logic.decide(identity)
assert(result.get("decided") && result.get("valid"))
```

`std.math.logic.prolog` is a self-hosted logic-programming engine with explicit
variables, compound terms, facts, rules, occurs-checking unification, recursive
backtracking, and bounded queries. Query results contain friendly `answers`
for variables from the original goal, plus raw `solutions`, step count, peak
depth, logical workspace usage, reason, and `decided`. Step, solution, depth,
variable, and workspace limits are explicit; hitting one returns
`decided=false`.

```ny
use std.math.logic.prolog as prolog

def X = prolog.variable("X")
def kb = [prolog.fact(prolog.term("likes", ["ada", "math"]))]
def result = prolog.query(kb, prolog.term("likes", ["ada", X]))
print(result.get("answers")) ; [{X: math}]
```

`std.math.logic.rewrite` reuses those terms for deterministic normalization:

```ny
use std.math.logic.prolog as prolog
use std.math.logic.rewrite as rewrite

def X = prolog.variable("X")
def zero = prolog.term("zero")
def rules = [rewrite.rule(prolog.term("add", [X, zero]), X)]
def result = rewrite.normalize(prolog.term("add", [42, zero]), rules)
assert(result.get("decided") && result.get("value") == 42)
```

Normalization reports its reason, passes, steps, and visited nodes. Pass, step,
depth, and node limits are explicit, and exhaustion returns `decided=false`.

Short names in this table continue the namespace shown earlier in the same row.

## Flat APIs

Many modules export flat function names. Import aliases namespace those names;
they do not create methods that are not exported.

### Time

`std.os.time` exports `time`, `now`, `unix`, `now_ms`, `sleep`, `msleep`,
`ticks`, `monotonic_ns`, `Instant`, `Timer`, elapsed helpers, and
`format_time`.

Use `msleep(ms)` for millisecond sleeps. Use `now_ms()` for wall-clock
milliseconds and `ticks()` or `monotonic_ns()` for elapsed-time measurements.

### Threads

```ny
use std.core
use std.os.thread

fn worker(any arg) any { arg }

def handle = thread_spawn(worker, "ok")
assert_eq(thread_join(handle), "ok", "thread")
```

`thread.spawn` is not exported.

### Parallel

Use `future(work, arg)` or `async(work, arg)` for joinable work, then
`future_wait(handle)`. Use `detach(work, arg)` for fire-and-forget work.
`parallel_map` and `parallel_map_indexed` preserve input order.

`fork`, `task`, and `join` are not exported names in this module.

## UI Notes

Use [ui.md](ui.md) for a first window, frame loop, drawing, input, textures,
and 3D start. Lower-level Vulkan modules are for renderer implementation and
targeted probes, not normal app setup.

## 3D References

- [Khronos glTF](https://github.com/KhronosGroup/glTF)
- [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets)
- [Needle Asset Explorer](https://asset-explorer.needle.tools/)

## Generated Reference

Generated docs own signatures and docstrings:

```bash
ny doc search --symbols json_decode
ny doc get std.math.parse.data.json
```

Related guides:

- [networking.md](networking.md)
- [native.md](native.md)
- [metaprogramming.md](metaprogramming.md)
- [ui.md](ui.md)
