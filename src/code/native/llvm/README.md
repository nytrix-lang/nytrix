# LLVM backends

The LLVM implementation has two deliberately separated layers:

- `emitter.c` is the unified NYIR emitter used by the native pipeline.
- `legacy/` is the compatibility AST-to-LLVM implementation used by the
  legacy JIT/default-backend path.

The compatibility backend is supported, but it is not the place for new NYIR
lowering work. Keep backend-specific helpers and tests beside the backend they
serve so the two implementations do not silently grow a shared fallback.

The target-machine and LLVM-C bridge in `legacy.c`/`legacy.h` is shared by both
layers. It stays at this level because the native emitter also uses its module,
optimization, target, object, and sanitizer helpers.
