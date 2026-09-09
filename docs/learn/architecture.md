<!-- nytrix-doc: {"audience":"contributor","featured":true,"group":"learn","order":190,"summary":"Comprehensive guide to Nytrix system architecture, 17 design principles, module boundaries, type system discipline, and subsystem seams."} -->
# Architecture & System Design

Nytrix is built around single-owner layers, deep modules, strict type discipline, and clear subsystem boundaries. This document outlines the architectural principles, design workflows, and invariant boundaries that govern the codebase.

## 1. Design Workflow: Design Before Implementation

Before implementing significant changes or refactoring subsystems:

1. **State the Problem**: Define the precise behavior that is broken, missing, or under-specified.
2. **Exhaust the Design Space**: Formulate and evaluate at least three concrete alternative approaches before writing code.
3. **Redesign from First Principles**: Avoid stacking patches or shims on top of structural defects; address the root cause in the owning layer.
4. **Subtract Before Adding**: Remove obsolete pathways, redundant abstractions, and dead state before introducing new machinery.
5. **Outcome-Oriented Execution**: Define measurable, verifiable success criteria and executable proofs upfront.

## 2. The 17 Core Design Principles

Nytrix development follows 17 design principles categorized across structural, code quality, and process axes:

### Structural Principles
- **Foundational Thinking**: Identify the single layer that truly owns each invariant, data structure, or lifecycle.
- **Redesign from First Principles**: Understand the root physics of a problem rather than patching symptoms.
- **Subtract Before You Add**: Keep the codebase lean by pruning obsolete abstractions and compatibility shims.
- **Exhaust the Design Space**: Systematically explore alternative representations and trade-offs.
- **Outcome-Oriented Execution**: Work backwards from executable behavior and observable correctness.

### Code Quality Principles
- **Laziness Protocol**: Defer computation and allocations until strictly necessary.
- **Minimize Reader Load**: Keep functions focused, naming precise, and interfaces self-documenting.
- **Type System Discipline**: Use the type system as a compile-time proof engine:
  - *Make illegal states unrepresentable* using algebraic sum types instead of bags of optional fields.
  - *Brand semantic primitives* (e.g., `UserId`, `ByteOffset`, `NYIRValue`) to prevent accidental interchange of raw primitives.
  - *Parse at boundaries*: External inputs (CLI flags, environment variables, JSON, IPC messages, RPC payloads) are untyped until validated and parsed by an explicit boundary function.
  - *Never lie to the type system*: Avoid unchecked casts, unsafe coercions, or assertions that bypass compiler verification.
  - *Exhaustive matching*: Compilers must strictly enforce exhaustive pattern matching over sum types and enums.
- **Boundary Discipline**: Keep subsystem interfaces explicit with typed enums and structs rather than magic strings or numbers.
- **Idempotent Operations**: Design transforms, lowering passes, and analyses such that repeated application produces stable fixed points.
- **Separate Shared State**: Eliminate global mutable state where it impedes parallelism, testing, or reasoning.

### Process Principles
- **Fix Root Causes**: Solve defects at the owning source rather than masking them downstream.
- **Prove It Works**: Validate using executable oracles, cold test runs, and proof of fast-path activation.
- **Guard Context**: Keep patches minimal, self-contained, and focused on the stated goal.
- **Build the Lever**: Create reusable diagnostics, oracles, and test fixtures that make future changes safer.
- **Experience First**: Preserve fast compiler turnaround, clean diagnostics, and intuitive developer tooling.

## 3. Subsystem Ownership & Boundaries

Every behavior in Nytrix has exactly one owning layer:

| Subsystem | Source Location | Owned Responsibilities |
| :--- | :--- | :--- |
| **Language Frontend** | `src/code/parse/`<br>`src/code/frontend/` | Grammar, AST construction, lexical scopes, traversal, and diagnostics. |
| **Typing & Semantics** | `src/code/typing/` | HM inference, refinement and solver integration, ownership, effects, and persisted semantic facts. |
| **Intermediate Representation** | `src/code/ir/ir.h`<br>`src/code/ir/` | Target-independent SSA optimization, canonical control flow, loop analysis, verifier authority. |
| **Native Code Generation** | `src/code/native/` | Machine form representations, register allocation, ABI lowering, object emission. |
| **LLVM Integration** | `src/code/native/llvm/` | NYIR-to-LLVM emission/JIT bridge plus the supported, quarantined AST compatibility backend in `llvm/legacy/`. |
| **FFI Frontend** | `src/code/ffi/` | Clang adapter and the in-tree C header lexer/parser. |
| **Runtime & Execution** | `src/code/runtime/` | Memory allocator, garbage collection, dynamic value dispatch, platform primitives. |
| **Build Pipeline** | `src/code/wire/` | Compiler stages, caching, bundle construction, and self-build dependency tracking. |
| **CLI & Tooling** | `src/cmd/` | Standalone CLI tools (`ny`, `test`, `fmt`, `fuzz`, `perf`, `doc`, `web`, `dap`, `lsp`). |
| **Standard Library** | `lib/` | Public user-facing modules, collections, operating system bindings, and networking. |

### `src/code` layout

```text
src/code/
├── parse/                 Lexer, AST parser, statement grammar, proofs
├── frontend/              Scope resolution, AST traversal, diagnostics
├── typing/                Types, HM/refinement inference, ownership, effects
│   └── pipeline/          Staged semantic validation and fact persistence
├── ir/                    Canonical NYIR, verifier, SSA, machine-independent analysis
│   └── opt/               SSA, memory, dead-code, scalar, and loop passes
├── native/                Unified native lowering and terminal backend
│   ├── lower/             NYIR-to-machine lowering helpers
│   ├── machine/           x86-64 and stack instruction emission
│   ├── object/            ELF/object encoders and relocations
│   └── llvm/              NYIR→LLVM emitter/JIT; supported AST compatibility path in legacy/
├── ffi/                   Clang bridge and in-tree C header frontend
│   └── c/                 C lexer and declaration parser
├── runtime/               Allocator, GC, numeric, OS, and native shims
├── wire/                  Build stages, cache, bundle, and pipeline orchestration
└── incremental/           Incremental compilation state and invalidation
```

### Boundary Invariants
- **No Shims or Fallbacks**: If a subsystem contract changes, update the owning layer and migrate all affected callers directly.
- **Strict Layer Separation**: Target-independent NYIR transforms must never depend on physical registers or calling conventions. Machine encoders must not reconstruct source-language semantics.
- **Audit Duplicated Representations**: Regularly audit and eliminate duplicated type encodings across HM inference, semantic types, monomorphization, NYIR, and machine form.

## 4. Architectural Decision Records (ADRs)

When proposing major architectural changes, document the decision with:
- **Problem Statement**: What behavior is broken, missing, or inefficient?
- **Alternatives Considered**: At least 3 distinct designs with concrete trade-offs.
- **Chosen Approach**: Detailed technical rationale for the selected design.
- **Trade-offs Accepted**: Explicit costs in complexity, memory, or compile-time latency.
- **Verification Plan**: Exact test suites, oracles, and benchmarks required to validate the change.
