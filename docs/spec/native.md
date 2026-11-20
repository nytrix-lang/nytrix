<!-- nytrix-doc: {"audience":"user","featured":true,"group":"spec","order":40,"summary":"The language and ABI contract for native compilation, linking, targets, and unsupported shapes."} -->
# Native

Rules for layouts, extern blocks, pointers, handles, strings, ownership, and
ABI behavior at native boundaries.

## Contents

- [Execution routes and target support](#running-native-code)
- [Layouts, declarations, and header imports](#layouts)
- [Pointers, ABI, and ownership](#pointers-and-handles)

## Running native code

Use `--native-only` to force the native execution path. Use `--run=aot` or
`--output` for a native executable. Use the default mode while iterating unless native-only
behavior itself is what you are checking.

For a source file, `--native-only` selects the in-memory native JIT unless
`--run=aot`, `--output=PATH`, or `--run=none` selects an AOT or compile-only
route. Program output appears only when the source prints. The low byte of the
top-level result becomes the process exit status. Use `--output=PATH` to save
an executable or `--run=aot` to compile and execute a native artifact.

Unsupported native shapes fail with a diagnostic; they never silently fall
back to a different execution mode.

## NYIR artifacts and native objects

On x86-64, save a validated binary NYIR bundle and run it later in the
debug VM:

```bash
ny --native-precompile=build/cache/program.nyrb myprog.ny
ny --nyr-run-bin=build/cache/program.nyrb
```

The bundle also contains the native data tables needed by its lowered code.
`--emit=nyr` writes text, not a reusable binary. Build a native executable
without reparsing the source:

```bash
ny --native-from-nyr=build/cache/program.nyrb --output=build/cache/program
```

The compiler links the Nytrix runtime. External libraries are not recorded in
the bundle; pass their `-L` and `-l` flags when building. Select another
supported target with `--native-backend` and `--host-triple` when needed.
Native object support remains target- and shape-dependent; unsupported output
fails with a diagnostic.

New bundles use NYIP v2. Older v1 bundles remain runnable in the VM but do not
contain the native data tables required for object emission. Bundle capacity is
checked before output is written; a program is never silently truncated.

The public NYIR construction, verification, optimization, and native object
API is documented in the [Nyr API reference](nyr.md). Its current
object-emission target limits are narrower than the full in-tree native backend.

## Layouts

> **Note:** `Name` and `Type` are placeholders; this is not a complete layout
> declaration.

```ny
layout Name {
   Type field,
   Type field2
}
```

Field order is part of the ABI. Reordering fields changes the ABI. On the LLVM
route, a by-value FFI parameter or result requires a fully materialized LLVM
type matching the layout; unresolved layout types are rejected rather than
represented by an empty struct. Pointer parameters may use incomplete layouts.

## Extern blocks

> **Note:** The library and symbol names are placeholders. Use concrete ABI
> types and link the named library in a complete program.

```ny
extern "library" {
   fn symbol(Type arg) Type
}

extern {
   fn process_symbol(Type arg) Type
}
```

This declaration runs only when the named library exports the requested
symbol with the declared ABI and the build links it. The syntax itself does
not provide a symbol. Native examples need a build command that links the
selected library.

A bare `extern` names symbols from the current process. The library string
selects which linked or loaded library provides the symbol.

## Header imports

`#include` imports declarations from C headers:

```ny
#include <stdlib.h>
#include "./ffi.h"
```

An optional namespace alias exposes declarations under that name:

```ny
#include <stdlib.h> as "c"
c.malloc(64)
```

Without `as`, constants, macros, layouts, and non-conflicting functions are
available directly. Existing Nytrix names win on collisions.

### C frontend selector

| `--c-frontend` | Behavior |
|---|---|
| *(default)* | Internal frontend for diagnostics; libclang does lowering |
| `nytrix` | Internal frontend does all lowering; no libclang fallback |
| `libclang` | Always use libclang; skip the internal frontend |

### Include resolution

1. `"path"` resolves relative to the current file.
2. `<path>` searches `/usr/include`, `/usr/local/include`, and platform paths.
3. Max depth is 64. Recursive includes inherit the parser's typedef, tag, and
 macro state.

### Supported C header constructs

`#include` imports these declarations. The standalone C compiler has separate
function-body support.

Types: `void`, `_Bool`/`bool`, `char`, `short`, `int`, `long`, `float`,
`double`, `long double`, `_Complex`/`_Imaginary` (base type), `_BitInt(N)`,
`signed`/`unsigned`, `const`, `volatile`, `restrict`, `_Atomic`.

Declarators: pointers, arrays (flexible and unknown-size), function
parameters (variadic), deep function pointer nesting, typedefs.

Declarations: `extern`, `static`, `inline`; struct, union, enum with
fields, bitfields, anonymous members, and nested definitions;
`#pragma pack(push, N)` / `#pragma pack(pop)`.

Attributes: `packed`, `aligned(N)`, `unused`, `deprecated`, `weak`,
`format(...)`, `visibility(...)`, `alloc_size(...)`, `const`, `pure`,
`noreturn`, `malloc`, `warn_unused_result`, `vector_size(...)`, `__declspec`,
`_Alignas`, `_Static_assert`, `__asm__`, `__extension__`.

Preprocessor: object-like and function-like `#define` with `__VA_ARGS__`
expansion, `#undef`, `#if`/`#ifdef`/`#ifndef`/`#elif`, `#include` with
recursive resolution, `__has_include`, `__has_builtin`, predefined platform
macros.

### Unsupported

- Function definitions (prototypes only)
- Statements, expressions, compound literals, designated initializers
- `_Generic` selections

### C-to-Nytrix type mapping

| C type | Nytrix | C type | Nytrix |
|---|---|---|---|
| `void` | `void` | `_Bool`/`bool` | `u8` |
| `char` | `i8`/`u8` | `short` | `i16`/`u16` |
| `int` | `i32`/`u32` | `long` | `i64`/`u64` |
| `float` | `f32` | `double` | `f64` |
| `long double` | `f64` | `enum` | `i32` |
| pointers | `ptr` | function pointers | `fnptr` |
| struct/union | named type | arrays | `ptr` |

Use `&value` when a C function expects an out pointer:

```ny
#include <sys/time.h>
mut timeval tv = timeval(0, 0)
gettimeofday(&tv, nil)
```

`NULL` lowers to `nil`/`0`.

## Pointers and handles

`*T` is an addressable pointer to `T`. `handle` is an opaque native scalar -
not a pointer unless the API documents the conversion.

## Calling convention

Typed native scalar integers use the Nytrix `i64` value representation.
In x86-64 NYIR emission, `u8`/`bool` parameters are read from the ABI
argument's low byte and zero-extended; `i8` parameters are sign-extended before
NYIR operations. This applies to register and stack arguments. Other scalar
integers and `f64` retain their existing raw value representation; dynamic
values keep their tagged representation through `any` boundaries. Layout
values cross by field in declaration order, which is part of the ABI. Function
pointers use `fnptr`. Variadic foreign calls follow the target calling
convention and pass arguments positionally; the exact varargs behavior belongs
to the selected backend and is checked at the boundary.

Foreign declarations require a matching symbol and native link configuration;
the REPL does not link arbitrary external libraries.

```ny
extern "library" {
   fn host_call(int a, f64 b) int
}

def out = host_call(1, 2.0)
```

A minimal native program checks a C-library call through its declared ABI.
Save the source as `ffi.ny` and run it on a host with the C runtime available:

```bash
ny --run=aot ffi.ny
```

```ny
use core

extern "c" {
   fn abs(int value) int
}

#main {
   assert_eq(abs(-42), 42, "libc boundary")
}
```

For the portable WebAssembly backend, scalar `extern "c"` calls import from
the `env` module. Parameters and results may be `i64`, `f32`, or `f64`. The
host must provide matching exports. Unsupported runtime helpers and
aggregate/vector signatures are rejected during emission.

### Standalone C frontend subset

The standalone `nyc` compiler accepts a bounded C subset.
`nyc -c SOURCE.c -o OUTPUT.o` or `nyc --all SOURCE.c OUTPUT.o` lowers all
function definitions in one source unit into a single x86-64 ELF object with
their C symbol names. Whole-unit object mode currently requires the SysV
x86-64 ABI; `nyc -S SOURCE.c -o OUTPUT.s` also supports i386 GNU assembly.
Legacy `nyc SOURCE.c OUTPUT.o SYMBOL` emits one named function.
On supported Linux x86-64 builds, `ny nyc OPTIONS...` dispatches to the
separately linked `nyc` executable; direct `nyc OPTIONS...` remains available.
`nyc --help` and `./make nyc -h` expose the same complete compiler flag
catalog with Ny's shared help styling. The catalog distinguishes active
code-generation controls from recognized defaults and compatibility-only
switches, and includes target constraints and compiler-swap examples.
NYC does not link executables or libraries: use a separate platform link
driver after compilation. It does not support the full C language and is not
a drop-in GCC/Clang driver replacement. Scalar
values include signed and unsigned integer widths through 64 bits, `float`,
and `double`. Narrow integer parameters, results, locals, and assignments
retain their C width. Explicit `signed char` and signed `short` loads
sign-extend; their unsigned counterparts zero-extend. Bare `void` is valid
only as a function result. Typed pointers support arithmetic, dereference,
indexing, and stores for these scalar types. Object-pointer relational
comparisons use unsigned address ordering.
Address-taken automatic `float` and `double` locals are accessed through their
stack homes after the address escapes, preserving writes made through the
pointer and floating-point call argument classification.
`int *` supports parameter passing, dereference, indexed loads, and
indexed or direct stores using signed 32-bit memory operations.
`unsigned int` supports ABI values, 32-bit wrapping arithmetic, and unsigned
relational comparisons, division, remainder, and right shift.
`unsigned long long` values support ABI passing, addition,
subtraction, multiplication, bitwise operations, and correctly ordered
relational comparisons; suffixed 64-bit integer literals retain their unsigned
bit pattern. Unsigned division, remainder, and right shift preserve that
unsigned interpretation.
`char *` indexing and
dereference use byte-width memory operations; byte loads currently zero-extend,
so high-bit values do not yet match signed `char` semantics on x86-64.
File-scope `char` arrays initialized by adjacent string literals are emitted as
data symbols and can be indexed from compiled functions.
File-scope signed/unsigned integer, pointer, and `double` objects support
zero/numeric initialization, reads, and simple writes. Emitted scalar and
aggregate objects are aligned to their natural C type alignment; initialized
file-scope aggregates and static aggregate locals retain that alignment through
whole-unit emission. Integer globals also support compound assignment;
block-scope scalar `extern` declarations resolve to C data symbols supplied by
the linker.
Character-pointer globals can use string literals, including adjacent literals.
Mutable integer and pointer globals support prefix and postfix updates;
`const` on a pointee does not make the pointer itself constant.
Whole-unit mode also emits initialized file-scope aggregates and initialized
static aggregate locals with persistent storage. Static scalar locals retain
their initialized storage and address across calls; narrow pointer writes are
read back at the declared width. This does not imply support
for arbitrary C aggregate initializers or aggregate members.
Automatic aggregate brace initializers can evaluate runtime expressions for
supported fields and arrays, including nested aggregates and bitfields.
Omitted elements are zero-initialized; literal-only initializers retain their
constant-data path.
Fixed arrays of pointers in C aggregate members support indexed insertions,
stores, and loads, including the `JsonCache`-shaped layout covered by the
frontend regression.
Constant `sizeof` initializers use resolved primitive and aggregate layouts,
including typedef aliases, rather than a default pointer-sized value.
Fixed-size automatic arrays and aggregate temporaries reserve stack storage
once per function invocation, not on every loop iteration. Their initialization
and other side effects still execute at the declaration or call site.
File-scope `typedef` aliases support signed `int`, signed `long`, `float`,
`double`, `void`, tagged struct/union types, and function-pointer values,
including alias chains and `const` qualification. A `void` typedef may be used
as an opaque pointer type in supported aggregate fields. `wchar_t` and
`wint_t` use the Linux x86-64 32-bit ABI types. Opaque function pointers and
pointers-to-pointers can be passed, returned, and stored in supported
aggregate fields, including indirect calls. File-scope function declarations
accept GNU `nothrow`, `leaf`, `cold`, and `hot` attributes as ignored
optimization hints. GNU `malloc` attributes, including deallocator arguments,
and `access` attributes are parsed but do not affect optimization or
diagnostics.
SysV x86-64 calls can pass scalar values to declared
variadic functions, including the default promotions for `float` and narrow
integers. Variadic definitions support scalar and pointer extraction through
`va_start`, `va_arg`, and `va_end`, using the SysV register-save area and
overflow stack.
Tagged aggregates support scalar fields, nested aggregates, arrays, pointer
tables, and bitfields. Bitfield updates preserve neighboring bits; signed
bitfield reads sign-extend. Member-address expressions do not load the
addressed object. Bare `char` still uses zero-extending byte semantics, unlike
the default signed `char` on x86-64; use explicit `signed char` when required.
Unsupported aggregate members, nonconstant initializers, and
other unsupported file-scope data forms remain rejected. The registered
acceptance test links and runs a multi-function NYR object from a C
harness. Separate translation units may define colliding file-scope `static`
functions and objects; NYC emits them with local ELF binding, covered by a
link-and-run regression. This does not establish full C compatibility.

The unmodified SQLite 3.46.0 amalgamation compiles in whole-unit mode.
An NYR-compiled API driver can create, query, and close an in-memory database.
SQL comparisons against GCC cover transactions, joins, indexes, window
functions, recursive queries, JSON, blobs, and a 1,000-row disk database with
updates, rollback, `VACUUM`, integrity checks, and reopening. The comparison
shell is GCC-compiled; the SQLite library is NYR-compiled. A prior NYC
`SQLITE_TEST` fixture run passed `expert1.test` 89/89 and `intck1.test` 70/70,
but later failed at `alter-3.1.1`. That failure traced to casted aggregate
pointer indexing using an 8-byte stride instead of the aggregate size. NYC now
lowers the index as an aggregate lvalue, covered by a Vdbe/Mem-shaped
compile-link-run regression. The current test-enabled NYC core compiles directly
from the amalgamation and links into the upstream fixture with compiler-rt and
non-PIE flags. `alter.test` passes 120/120, including `alter-3.1.1`, and
`ext/intck/intck1.test` passes 70/70. The prior `extraquick.test` failure at
`autoindex1-900` was traced to mixed pointer/aggregate local declarators being
lowered as scalar locals, leaving the aggregate copy destination uninitialized.
The C frontend now routes pointer-first declarations containing a later
aggregate declarator through aggregate-local lowering; a focused
compile-link-run regression covers this shape. Rebuilt with the current NYC,
`autoindex1.test` passes 37/37. `extraquick.test` now progresses beyond that
case but aborts at `json101-4.2` with heap corruption; Valgrind reports an
invalid free from `jsonCacheInsert` in the lookaside allocation block. A
register-allocation-disabled core reproduces this failure. The configured
Makefile route with system headers reaches unsupported `_Float32` declarations
in glibc; an earlier `-U__GNUC__` workaround failed at SQLite's `__isnanf`
argument type mismatch. End-to-end SQLite acceptance remains open.
These checks are not a performance claim.
NYC handles the shell's nested signed `char **` indexing/assignment,
pointer-to-pointer stores, `sizeof(pointer[index])`, local character-array
string initialization, block-scope enum definitions, and additional hosted
declarations. The NYC-compiled shell and core link and run a basic scenario
that prints SQLite 3.46.0's version, creates a table, inserts `42`, and selects
it. This smoke does not establish full shell behavior or pass the upstream
SQLite test suite.

Use `-I DIR` to add include search paths. Quoted includes first search the
containing file's directory; angle includes search the configured paths.
Nested headers share macro state, and include guards and `#pragma once` work
across repeated and equivalent paths. Missing headers, active `#error`
directives, and unbalanced file-local conditionals are errors. The compiler
checks its modeled standard headers before native system-header fallback.
On Linux, the standalone driver with its default native target then searches
`/usr/local/include`, the host architecture's multiarch include directory,
and `/usr/include`. `-nostdinc` disables this fallback; explicit targets and
embedded frontend users do not enable it implicitly. Explicit `-I` paths still
take precedence. Modeled standard headers remain a bounded ABI surface, not
full libc headers; finding a system header does not establish support for
every declaration it contains.
The modeled `<stdio.h>` declares variadic `printf`. External character-array
declarations can be repeated and completed by a compatible definition.
The modeled `<stdbool.h>` defines `bool` as `unsigned char` and provides
`true`/`false` macros; this is a bounded compatibility alias, not full `_Bool`
conversion semantics. The standalone frontend also models the common
`ctype.h`, `string.h`, `stdlib.h`, and `sys/stat.h` declarations used by hosted
C utilities.

`nyc -E [-P] [-I DIR] [-D NAME[=VALUE]] [-U NAME] [-o FILE] SOURCE.c`
preprocesses without compiling declarations or generating code. Use `-x c -`
for standard input. `-P` omits line markers; otherwise markers retain header
filenames and source lines. Command-line object macros apply in argument
order. `-o` also selects the output path in object-emission modes.
`-O0` through `-O3` select the NYR optimization level; the default is `-O2`.
`-Os` currently selects the same conservative NYR `-O2` pipeline; NYC does
not yet use a distinct size-optimization pipeline.
At O1 and above, locally bound helpers enter the existing heuristic
inliner, including section-attributed and retained definitions. Static
functions bind locally; nonweak global functions also qualify outside PIC
mode. Interposable PIC globals and weak definitions do not. Retaining
an out-of-line definition does not prevent cloning its body at a call site.
GNU `noinline` and
`__noinline__` attributes are preserved across prototypes and definitions;
explicit `inline` linkage metadata remains separate from automatic eligibility.
Inline assembly that defines named global symbols also prevents cloning
the containing function, avoiding duplicate symbol definitions.
Functions using `va_start` or `va_arg` are not inline candidates because those
operations depend on the original call frame.
`-pthread` defines `_REENTRANT=1` before explicit command-line macro options,
so `-U_REENTRANT` takes precedence in either argument order. NYC does not
link executables itself; the external link driver must also receive `-pthread`.
Whole-unit SysV x86-64 compilation supports `-fstack-protector`,
`-fstack-protector-strong`, `-fstack-protector-all`, and
`-fno-stack-protector`, with the last option taking precedence. The default
mode conservatively protects native frame objects of at least eight bytes;
strong mode also protects address-taken locals, and all mode protects scalar
functions. Functions with no normal return need no return-cookie check.
GNU `no_stack_protector` attributes survive declaration merging and disable
instrumentation for that function, including under all mode.
The compiler initializes a dedicated frame word above addressable storage,
checks it at every normal return, and calls `__stack_chk_fail` on mismatch.
The check accesses that fixed frame word directly, never through a pointer
spilled before user code. Guards default to FS offset 40. The
`-mstack-protector-guard=tls|global`, `-mstack-protector-guard-reg=fs|gs`,
`-mstack-protector-guard-offset=NUMBER`, and
`-mstack-protector-guard-symbol=IDENTIFIER` options select the source;
TLS symbols override numeric offsets. The matching GNU `__SSP__`,
`__SSP_STRONG__`, or `__SSP_ALL__` predefine precedes explicit `-D`/`-U`
options. Assembly and direct ELF routes have runtime corruption checks for
FS, GS, numeric/symbolic TLS offsets, global guards, and opt-outs. Other
target ABIs and single-function compilation reject enabled stack protection.
This is not general `__seg_fs`/`__seg_gs` C address-space support, and the
physical machine pipeline still needs guard-word lowering before its
stack-encoder fallback can be removed.
`nyc -c SOURCE.c -o OUTPUT.o` is the whole-unit object route. Options may
appear before or after the input; `--` ends option parsing. The object driver
also accepts repeated `-include HEADER` options and Kbuild's
`-Wp,-MMD,FILE` / `-Wp,-MD,FILE` dependency-file forms. Dependency output lists
filesystem source and header files loaded by preprocessing, including forced
headers with no emitted tokens. The main source is the first prerequisite,
including when forced headers precede it during preprocessing; Linux
`fixdep` uses that ordering to identify the translation unit.
`-fmacro-prefix-map=OLD=NEW` rewrites
`__FILE__` and `__BASE_FILE__`; when multiple prefixes match, the last matching
option wins, matching GCC. `-m64`, `-march=x86-64`, and `-mcmodel=kernel`
require an x86-64 target. SIMD-disable flags require an x86 target, including
i386. `-m32 -S` emits i386 GNU assembly; assemble and link it with a 32-bit
external toolchain. This route supports stack-passed aggregate values,
floating-point argument placement, and SysV hidden-pointer aggregate results,
including the callee-pop result-pointer convention. `-mregparm=0` through
`-mregparm=3` select i386 argument registers, including small integer aggregates
and 64-bit scalar register pairs. Floating-point scalars and single-real-member
records retain their stack placement; an aggregate that exhausts the remaining
register bank also forces later integer arguments to the stack. Hidden result
pointers use the first available argument register and are not callee-popped
when register-passed. GNU `regparm(N)` function attributes override the default;
variadic declarations use stack arguments. Freestanding 32-bit consumers check
all four register counts, callbacks, explicit cdecl overrides, scalar 64-bit
values, aggregate copies, and aggregate results across the host/NYC boundary.
Variadic definitions use the i386 pointer-shaped `va_list`, including
`va_copy`, by-value list arguments, mixed-width extraction, fixed aggregate
parameters and hidden result pointers. With nonzero `-mregparm`, variadic
aggregate results follow GCC's caller-pop convention; Clang's default differs,
so these GNU-option interoperability checks use GCC. x87 integer conversions
preserve all 64 bits and truncate toward zero without changing the caller's
control word.
Automatic aggregate-pointer arrays retain pointer-sized elements, including
initialized and uninitialized arrays. Their i386 loads, stores, and strides
use four bytes. `sizeof`, alignment expressions, and
`__builtin_offsetof` have the target's unsigned `size_t` width; cross-compiler
varargs consumers verify their i386 argument placement.
File-scope tentative and incomplete pointer arrays can be completed by a later
definition, with unspecified initializer elements zero-filled. GNU empty array
initializers and zero-length outer bounds retain their zero size. Character
array string initializers support signed and unsigned element types.
`-m16` uses this ABI with GNU `.code16gcc` assembly.
`-mpreferred-stack-boundary=3`
is accepted for Linux Kbuild; NYC keeps its 16-byte frame and call alignment.
Warning switches are accepted but do not provide GCC-equivalent warning
diagnostics. Options requiring
unsupported semantic behavior are rejected rather than accepted as silent
no-ops. This driver compatibility is not evidence of kernel build
compatibility.
In `-c` whole-unit mode, `-fomit-frame-pointer` emits RSP-relative x86-64
frames, including non-leaf calls, spilled values, aggregate homes,
callee-saved registers, and incoming stack arguments. Functions containing
dynamic `alloca` or variadic-list operations retain an RBP frame so changing
`%rsp` cannot invalidate their frame references. A compile-link-run regression
exercises leaf and calling functions with stack arguments and register
preservation; the variadic translation-unit regression exercises the fallback
boundary. The option is rejected for the single-function object route.
The whole-unit route also supports `-mindirect-branch=thunk-extern` and
`-mindirect-branch=thunk-inline` for indirect calls, using `%r11` as the target
register, with `-mindirect-branch-register` accepted alongside either mode.
The external mode requires the linked object set to provide
`__x86_indirect_thunk_r11`; inline mode emits a local retpoline sequence.
These retpoline options are rejected by the single-function object route.
External function declarations with GNU `asm` labels are parsed for header
compatibility, but calls, addresses, and static initializers through those
aliases are rejected because the alias symbol is not yet lowered.

The driver implements `-fshort-wchar` (`__WCHAR_TYPE__`,
`__SIZEOF_WCHAR_T__`, `__WCHAR_WIDTH__`, and `wchar_t` sizing) and
`-funsigned-char`; default plain `char` is signed on x86-64. The C optimizer
does not use type-based alias assumptions, so `-fno-strict-aliasing` is
accepted and covered by an aliasing runtime probe.

Build probes can identify the frontend with `__NYC__` and the numeric
`__NYC_MAJOR__`, `__NYC_MINOR__`, and `__NYC_PATCHLEVEL__` macros.
`nyc --version` reports the build identity. The frontend defines `__STDC__` as
1 and GCC
compatibility version macros as `__GNUC__ == 12`, `__GNUC_MINOR__ == 0`, and
`__GNUC_PATCHLEVEL__ == 0` for GNU-compatible headers; it does not define
`__clang__`. This version is a compatibility declaration, not a claim of full
GCC 12 language or code-generation compatibility.

Modeled headers use the same include cache, guards, macro expansion and
source tracking as filesystem headers. Linux x86-64 declarations include
`passwd`, `dirent`, `utimbuf`, and `rusage`; their record layouts and libc
calls are checked across the GCC/NYR boundary. These declarations do not
establish portability to other libc ABIs.

Linux 7.3 Kbuild integration is experimental. Direct Kbuild can
identify NYC and run the compiler-driver route. Anonymous struct/union members
are supported, including promoted member access; the standalone frontend
compiles and runs `scripts/basic/fixdep.c` with output matching GCC. NYC also
emits x86-64 `-S` assembly for supported C units. Linux's time-constant generator
requires `bc` on `PATH`; an unpacked host-tool package is sufficient without
changing the host installation. Direct Kbuild with NYC
generates and assembles `scripts/mod/devicetable-offsets.s`, passes modpost, and
generates time constants. A `call target 'if' must be declared` diagnostic from
`alternative.h` was fixed and verified with a reduced compile/link/run
regression for the GNU statement-expression control flow. NYC also handles
anonymous aggregate `sizeof` and no-argument `aligned`, with a compile-time
size regression. The latest direct `prepare` retry generated device-table offsets, passed
modpost, and generated time constants, then stopped at
`arch/x86/include/asm/cmpxchg_64.h:52:55` because `union __u128_halves`
aggregate initialization does not match the parsed field type. Earlier blockers
in the same path are resolved: `native_save_fl`'s `=rm` output lowers to local
memory; unused static functions are no longer retained because their names
appear in diagnostic strings; `typeof` handles member expressions; GNU `void *`
arithmetic uses byte scaling; and `__builtin_types_compatible_p` lowers in
function expressions. Regressions cover static-asm pruning, volatile member
stores, and void-pointer comparison after arithmetic. GNU switch case ranges
are supported and covered at their lower/upper boundaries. Nested local
aggregate initializers now copy parenthesized aggregate expressions into
storage. Linux 7.3 `prepare` currently stops at `include/linux/find.h:67` on an
unsupported incomplete type in `sizeof`.
Linux 6.9 `x86_64 defconfig` also generates. Its direct
`prepare` passes the `__indirect_branch__` function attribute and asm-goto
labels in statement expressions, merges compatible repeated external
aggregate-pointer arrays, parses their multidimensional form, and handles
pointer arithmetic inside `typeof`; regressions check array row stride and the
`typeof` result. NYC now lowers `native_save_fl`'s `=rm` output to a local
memory slot, aggregate compound literals, `__builtin_choose_expr` expression
branches, address-of one-byte aggregate array members, and
`alternative_input`'s immediate-plus-`%P1` memory operands. Linux 6.9 `prepare`
now passes the x86 `cmpxchg_64.h` initializer and stops at
`include/linux/thread_info.h:239` because `__builtin_object_size` is treated as
an undeclared call.
Full `vmlinux` remains unsupported because Linux exercises
operand-bearing inline assembly and array declarators in its kfifo types.
NYC's constant evaluator handles unsigned 64-bit wrap in Linux's
`GENMASK_ULL` assertions and the frontend lowers unsigned 64-bit
`__builtin_add_overflow`, `__builtin_sub_overflow`, and
`__builtin_mul_overflow`. The frontend also lowers `__builtin_clzl`,
`__builtin_clz`, and `__builtin_ctzl`; runtime coverage checks boundary and
zero inputs.
NYC preprocesses
`init/main.i`, including
requested dependency output. C parsing now handles file-scope `_Static_assert`, `_Bool`, `__signed__`,
aligned aggregate fields/types (including attributes between the type and
field name), `warn_unused_result`, and multiple external integer-array
declarators. Integer constant expressions support `__builtin_choose_expr`
when both alternatives are integer constant expressions, and `sizeof` handles
scalar C expressions plus dereferenced pointer conditional expressions used
by Linux's `__is_constexpr`.
Unnamed integer bit-fields now preserve their padding and member offsets; the C
regression compares NYC output against host C. The no-SSE
scalar-integer path is tested to emit no MMX/SSE/AVX registers; floating-point
and SIMD code fails closed in that mode.
`-mcmodel=kernel` and `-mcmodel=small` are accepted because the x86-64 emitter
uses RIP-relative symbol references. The C frontend linker regression confirms
that an in-range kernel-address data reference links and an out-of-range
reference is rejected by the final linker; this does not validate a full
kernel link. The object backend emits 16-byte-aligned function
entries by default, so `-falign-functions=16` requests that verified layout.
The standalone NYC C object writer supports `-fomit-frame-pointer` for
whole-unit x86-64 objects, including leaf and call-containing functions, with
RSP-relative homes and call-stack adjustments. Variadic-list and dynamic
`alloca` functions retain RBP frames; the single-function object route rejects
the option. Compile-link-run regressions cover stack arguments, spills, and
callee-saved state, alongside the emitted `.text` alignment for
`-falign-functions=16`.

A prior local-adapter Kbuild run passed preparation and
`checksyscalls.sh`, compiled `scripts/mod/empty.o`, and reached NYC compilation
of `init/main.o`. GNU scalar statement expressions and `__builtin_expect` now
parse and lower, and local-static pointer relocations plus function addresses
have linked runtime coverage. That partial build stopped at
`arch/x86/include/asm/bug.h:160` on the operand-bearing
`lea (2f)(%%rip), %[addr]` assembly with output constraint `"=r"`. NYC's exact
`leaq %c1(%%rip), %0` pointer helper is lowered as pointer identity; this is
not generic inline-assembly emission. NYC also lowers operand-free x86-64
statements for `pause` and `nop`, plus empty templates with an optional
`"memory"` clobber. The memory-clobber operation is a volatile NYIR effect
barrier with no emitted bytes. Adjacent string literals in the supported
constraint and clobber positions are concatenated. Regressions link and run
these forms and verify the `pause` encoding. General operand-bearing
constraints and asm expressions remain unsupported. `asm goto` in a
reachable function remains unsupported; an unreferenced static-inline
function containing it is discarded.
A fresh NYC `defconfig` passes Kconfig's assembler-version query through
`-Wa,--version` forwarding. NYC accepts `-x assembler` and
`-x assembler-with-cpp` for source files and standard input, with GNU `as`
for object output. `-Wa,--fatal-warnings` keeps assembler warnings fatal.
The earlier scratch Linux Makefile supplied external
retpolines for kernel objects and inline retpolines for VDSO objects when
`CONFIG_CC_IS_NYC` is selected. A current local-adapter Kbuild attempt with
retpoline enabled generates offsets and initramfs data and begins kernel C
compilation; Kbuild now passes the Hyper-V and `GENMASK_ULL` constant
assertions plus unsigned size overflow builtins. That local-adapter attempt
reached the per-CPU asm at `asm/preempt.h:37` and rejected its `"m"` input
constraint;
`init/main.c` has another output constraint and `include/linux/kfifo.h` retains
an array declarator blocker. Whole-unit `-fPIC`/`-fPIE`
emission now uses GOTPCREL for symbol addresses and PLT32 for direct calls.
`-Os` selects the documented conservative NYR `-O2` pipeline. Keep retpoline
enabled for acceptance.
File-scope assembly
supports section push/pop, `.section`, `.balign`, `.align`,
`.byte`, `.long symbol - .`, `.quad`, `.ascii`, `.asciz`, `.previous`, local
labels, and cross-section PC-relative relocations. Linked regressions check
the emitted relocation and data bytes. The frontend now handles external
function-pointer objects with section attributes, fixed and incomplete
string-pointer arrays with null entries, tentative character arrays, repeated
extern declarations after definitions, scalar declaration lists, nested
aggregate compound literals, scalar brace-enclosed initializers, and
self-referential aggregate member relocations. NYC now parses the
typedef-based `initcall_levels[]` pointer array and `initcall_level_names[]`
string array in `init/main.c`. General operand-bearing C inline-assembly
parsing and lowering remain unsupported; the file-scope tracepoint form has
not been reached by the latest local run.
Frontend and lexer suites cover continued
macro comments; enum forward references and prototypes; repeated external
declarations; pre-name alignment and section attributes; enum ternary
constants; constant `offsetof` and `__alignof__`; unsigned-int complement
width for literals and casts; builtin-based array bounds; and aggregate
pointer arrays. This Kbuild path uses a local adapter and does not establish
kernel acceptance; NYC todo/acceptance notes are tracked in
[NYC Linux kernel acceptance](../todo/nyc.md).
Whole-unit x86-64 objects accept `-fPIC` and `-fPIE`: symbol addresses use
GOTPCREL relocations and direct calls use PLT32 relocations. Compile-link-run
regressions exercise both options as shared objects and executables, including
external and local data addresses and external calls. The C frontend resolves
`typeof`/`__typeof__` of global register declarations and supports volatile
integer or pointer reads of an x86-64 global register variable bound to `rsp`;
its address cannot be taken, and writes or bindings to other registers remain
unsupported. The x86 Linux `ASM_CALL_CONSTRAINT` read-write `"+r"` form is
accepted for a global `rsp` binding only for a single direct `call` template
without operand placeholders; a compile-link-run regression covers this case.
The x86-64 `=rm` output form is also lowered for a single mutable 64-bit local
when the template has one `%0` memory placeholder; the `pushf; pop %0` path
has a compile-link-run check. This does not imply general operand-bearing asm
support.
Empty-template inline asm with a single `=r` output tied to a
`"0"` pointer or 64-bit integer input lowers as an identity copy; this narrow
case has compile-link-run regression coverage. Other operand-bearing C inline
assembly remains unsupported; unsupported asm in unreferenced static functions
is discarded. NYC's `-S` route emits and
runs x86-64 assembly, including common file-scope data/relocations and local or
sectioned functions. Special data-section flags remain unsupported.
Input-only `"i"` operands with constant expressions are substituted into
templates for this route, including the `order_base_2` immediate expression
used by Linux bounds generation. Earlier Linux 7.3 preparation runs generated
and assembled `scripts/mod/devicetable-offsets.s`, passed modpost, and generated
time constants with the cached `bc` package. A `for` initializer may carry a cleanup attribute, and early-clobber
`rep stos{b,w,l,q}` plus named-function `__builtin_types_compatible_p` checks
have compile-link-run coverage. A resumed clean `bzImage` and
modules acceptance build is still in progress; those preparation milestones do
not establish full-kernel artifact equivalence. Initialized block-scope
`__auto_type` and C23 `auto` declarations are supported for scalar and pointer
initializers. GNU `typeof` recognizes `__builtin_constant_p` and
`__builtin_ffs` call result types; function-pointer calls returning `void`
remain valueless inside conditional expressions; and preprocessing retains
branch state when a function-macro argument closes inside a conditional branch.
These paths have focused runtime regressions and allow three more Linux 7.3
objects to compile. Aggregate arrays also decay for arrow-member assignment,
and function-pointer arrays accept `((void *)0)` initializers; executable
frontend regressions cover both. Retpoline support, normal assembly integration, complete
compiler-flag semantics, and full-kernel artifact equivalence remain
unverified. Do not treat a partial Kbuild run or a configuration with
compiler/security features disabled as a kernel build.

File-scope enums support tagged and anonymous definitions, typedefs, explicit
integer constant expressions, automatic successors, array extents, and
function parameters and returns. Constants must fit `int`; empty definitions,
duplicate constants, and unsupported block-scope definitions are rejected.
Fixed-size integer arrays preserve their declared size, zero-initialize
omitted elements, and remain writable unless declared `const`. Character
arrays accept concatenated strings, zero padding, or an exact-width string
without a terminator. Array parameters decay through the shared pointer-type
rules, including arrays of character pointers and aggregate pointers.
File-scope arrays of pointers to tagged aggregates support fixed constant
extents and zero-initialized storage, including comma-separated declarators.
Nonzero aggregate-pointer array initializers remain unsupported.

The modeled `<assert.h>` checks `NDEBUG` on each inclusion. Disabled assertions
discard their arguments without a helper call; enabled assertions evaluate
once and abort on false. Casts to `void` retain operand side effects but do not
produce scalar or pointer initializer values.

Object-like and function-like macros expand recursively in declarations and
function bodies. Arguments expand before substitution except beside `#` and
`##`, which use their original tokens. Nested wrappers can therefore expand a
macro before stringifying or pasting it. Backslash-continued macro replacement
lists preserve embedded comments as whitespace rather than truncating at the
comment. `__LINE__` values remain stable when later macro expansions are
scanned.
It tracks active `#define` / `#undef` names for `#ifdef` / `#ifndef` and expands
object and function macros before evaluating `#if` or `#elif`. `defined`
operands are tested without expansion; macro aliases use their current values
and replacement tokens retain their precedence. The bounded expression grammar
supports integer and character literals, arithmetic,
shift, and bitwise operators, `defined(NAME)`, unary `!`, relational and
equality comparisons, `&&`, `||`, and parentheses. Simple integer macro aliases
are not frozen when defined. Logical operators short-circuit evaluation, and
an already-taken conditional branch skips later `#elif` expressions.
Recognized `__has_*` compiler feature probes evaluate false. Inactive nested
conditional regions are skipped without parsing their C tokens.

### Aggregate ABI coverage in the C frontend

The standalone C frontend classifies and lowers by-value aggregate parameters
and returns using the selected target ABI. The supported aggregate classifiers
are:

| Target ABI | Supported classes | Fail-closed boundary |
| --- | --- | --- |
| SysV x86-64 | INTEGER/SSE eightbyte mixes through two eightbytes; larger values use memory classification | Unsupported field layouts or class shapes are rejected. |
| AAPCS64 ELF | Homogeneous floating aggregates of one to four `f64` members; integer aggregates through 16 bytes, including 16-byte-aligned values; larger values use memory classification | Unsupported aggregate ABI targets, including AArch64 Windows, are rejected. |
| Win64 x86-64 | One-eightbyte INTEGER aggregates are direct; larger by-value arguments and returns use the ABI's indirect form | The frontend's current direct aggregate field model is limited to eight-byte shapes; unsupported signatures are rejected. |

The C frontend acceptance suite compiles, links, and runs C caller/compiled
callee pairs for AAPCS64 under QEMU and Win64 under Wine when those tools are
available. The matrix covers direct and indirect arguments/returns, positional
argument boundaries, homogeneous aggregates, unions, and mixed classes. The
NYIR verifier and in-tree native/JIT route matrix for Nytrix layout values are
separate checks; passing the C frontend matrix does not imply that every C
aggregate shape is supported by every native backend.

## Layout helpers

> **Note:** `Name`, `storage`, and field values are placeholders; helper calls
> require a matching declared layout.

```ny
__layout_size("Name")
__layout_align("Name")
__layout_offset("Name", "field")
store_layout(storage, "Name", first_value, second_value)
load_layout(storage, "Name", "field")
```

Raw load/store helpers:

| Helper | Use |
|---|---|
| `load8`, `store8` | Raw bytes |
| `load16`, `load32`, `load64` | Tagged int/value |
| `load64_i`, `store64_i` | Typed integer |
| `load64_h`, `store64_h` | Handle/pointer |
| `load32_f32`, `load64_f64` | Float read |
| `store32_f32`, `store64_f64` | Float write |

Tag and word introspection: `__tagof(value)` returns the value's runtime
tag. Raw integers, floats, and `cstr` values resolve at compile time;
pointers, containers, and unknown shapes call the runtime. `__runtime_tag("Name")`
returns the tag of a declared runtime type name, so tests can pin tower
behavior with equality against the same helper:

```ny
assert(__tagof(a) == __runtime_tag("bigfloat"), "runtime tag")
assert(__tagof(promoted_add) == __runtime_tag("bigint"), "add overflow promotion")
```

`__handle_valid(h)` reports whether a handle refers to memory the runtime
knows: a typed-buffer handle, a dictionary map, or a readable heap
allocation. It returns false for `0` and unknown addresses. These helpers
exist for diagnostics, comptime reflection, and fixture assertions;
application code should use type predicates and conversion APIs instead of
tag words. The fixtures `etc/tests/lang/values/bigfloat.nshape`,
`etc/tests/lang/values/bigint-pidigits.nshape`, and
`etc/tests/lang/types/inference/stale_handle_validation.nshape`
verify these behaviors.

## Inline assembly

```ny
asm("mov $1, $0", "=r,r", value)
intrinsic("ctpop.i64", value)
```

`asm` emits target-specific code. `intrinsic` resolves a backend intrinsic.
Both are backend-specific; use ordinary operations when source must run
everywhere.

## Native target support tiers

Backend names do not imply equal production maturity. The source tree uses
these support tiers:

| Tier | Backends | Contract |
| --- | --- | --- |
| Primary native | `x86_64`, `aarch64` | Main native architecture families, with machine-form lowering and target-specific object infrastructure; feature coverage still depends on the target and source shape. |
| Secondary native | `x86`/i386, `arm` | Target/ABI lowering with narrower optimization, object, and ABI coverage than the primary tier. |
| Bring-up / debug | `riscv`, `bpf`, `mips`, `powerpc`, `avr` and the portable NYIR text emitters | Intended for bring-up, cross-checking, and diagnostics; not a promise of host-quality native-only parity. |
| Browser/Wasm | `wasm` | Separate browser/WebAssembly execution contract; do not infer native object/JIT parity from the backend selector. |

Capability checks determine support for each object format, JIT path, machine
feature, and ABI form. Unsupported requests must produce diagnostics.

## Machine form

NYIR lowers to typed machine form before target emitters. The target descriptor
owns register-assignment tables; machine form must use those tables rather than
embedding a calling convention. Scalar calls beyond the register set place
overflow arguments in aligned stack slots.

- `--nyr-check=oracle-per-pass`: compare interpreter and native result after
 every optimization pass.
- `--nyr-check=tv-seed=N`: translation-validation trials for pure i64 scalar
 NYIR after each optimization pass. Translation validation is off unless this
 flag is supplied; the flag without `=N` selects `16` trials, and `N=0` turns
 it back off.
- `--dump=nyr`: print optimized NYR.
- `--inspect`: show optimized IR and optimizer pass summaries.
- `--nyr-dump=cfg,raw,stats,passes`: request CFG, before/after snapshots,
  instruction summaries, and pass detail. `--inspect` is the concise
  inspection preset.
- `--nyr-check=verify`: enable verifier after every pass (useful for CI).
- `--nyr-pass=disable:NAME`, `--nyr-pass=stop-after:NAME`: diagnostic controls.

### Whole-program LLVM optimization

`--opt=lto` preserves optimization after independently compiled module bitcode
is linked into the program module. Pair it with an optimization level, for
example `ny --opt-level=3 --opt=lto --output=app main.ny`.

For a selected native backend, the same flag selects the aggressive
whole-program NYIR pipeline. It raises that pipeline to its highest level and
enables its interprocedural small-function inlining before machine lowering.
It is not LLVM bitcode LTO, and unsupported native shapes still fail with their
own diagnostic.

## Strings and bytes

FFI text handling is a boundary. APIs document whether strings are managed
Nytrix text, UTF-8 bytes, null-terminated native strings, or raw buffers with
explicit length. These forms are not interchangeable.

## FFI trust boundary

Foreign declarations are a trust boundary. Unknown foreign calls are assumed
to have externally observable effects and may read or mutate reachable memory.
The optimizer may refine that only from explicit, trusted declaration metadata
or contracts that the program has chosen to rely on. Imported C `const` and
`restrict` information is descriptive input; it is not permission to invent a
Nytrix ownership or noalias guarantee when the call boundary cannot justify it.

Use three labels when documenting an FFI assumption:

- **unknown** — no ownership/effect refinement is trusted; keep conservative
  effects and aliasing;
- **checked** — a Nytrix wrapper performs a runtime/structural check before
  exposing a narrower safe value; the optimizer may rely only on facts proved
  after that check;
- **trusted** — the declaration or wrapper asserts a property that cannot be
  checked cheaply (for example an external lifetime/noalias contract). A wrong
  assertion is a boundary bug and must not be presented as compiler inference.

Low-level code can still use raw `extern` declarations and imported C
declarations directly. Checked/trusted wrappers are an opt-in safety layer, not
a requirement that prevents direct ABI calls.

Pointers returned from foreign code are `unknown` ownership by default. Use an
explicit ownership contract when the API guarantees a borrowed, consumed, or
returned-owned lifetime, and keep the raw declaration available when no safe
contract can be stated. Callback userdata follows the same rule: the caller must
keep borrowed userdata alive for every callback invocation, while a transferred
value needs an explicit consuming/owned contract.

Unsafe assumptions should be concentrated at the extern/wrapper boundary. They
should not leak into ordinary optimizer metadata as if they had been proved by
Nytrix itself.

## Ownership

Ownership attributes define who allocates and who frees:

```ny
@returns_owned
fn make_buffer(){ malloc(64) }

@consumes(p)
@releases(p)
fn free_buffer(p) int {
   free(p)
   0
}
```

## Related

- [Types](types.md) - pointer and handle type forms.
- [Runtime](runtime.md) - ownership and resource scopes.
- [Native](../learn/native.md) - practical FFI checks.
