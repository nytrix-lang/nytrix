# Legacy LLVM backend

This directory contains the compatibility AST-to-LLVM backend. It remains a
supported backend and is deliberately kept separate from the unified NYIR
emitter in the parent `llvm/` directory.

Use it for the legacy LLVM AST compatibility path used by the REPL and
embedding callers. The ordinary CLI JIT now uses the unified NYIR emitter.
New native lowering work belongs in `src/code/native/` and `llvm/emitter.c`;
fixes needed only by this backend should stay in this directory and retain a
focused test.

The backend is split into:

- `core/`: lifecycle, lazy reachability, module links, emission, and disposal;
- `expr/`: AST expression lowering and floating-point lowering;
- `gencall/`: ABI marshalling, intrinsics, proofs, and specialization;
- the top-level units: symbols, functions, statements, modules, and operators.

The public bridge header remains `code/native/llvm/legacy.h` so callers can
select the compatibility backend without depending on internal layout.
