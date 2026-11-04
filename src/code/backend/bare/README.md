# Linux ELF64 string runtime payloads

`runtime.c` implements the selected raw-word runtime boundaries for the internal
x86-64 and AArch64 bare emitters. `payload.h` maps shared runtime-operation
identities to the private entry ABI: four argument words followed by an
operation number. Binary64 arguments are their bit patterns, even when the
source call has a floating-point ABI. The adapters load canonical word homes.

The checked-in `.inc` files contain position-independent code and read-only
constants, with no external references or mutable global data. A function that
uses these operations embeds one copy behind a jump, then calls its entry.
The ordinary SysV/AAPCS64 call preserves callee-saved registers; the payload
allocates its own stack scratch. Programs that do not use these operations
embed no payload. Bare compilation needs no assembler, C compiler, libc, or
external runtime. Other target/OS combinations remain unsupported.

Strings allocated here have the existing 32-byte bare header: magic, mapping
size, tagged byte length, and string tag 120, followed by a NUL-terminated
payload. Float boxes have tag 110 at payload minus eight. Builders own a wrapper
mapping and a separate raw buffer; growing preserves self-append offsets and
unmaps the old buffer. Finalization releases the wrapper and returns the raw
NUL-terminated buffer. Allocation/size failures return zero, except repeated
append retains the runtime's partial-append fallback.

Float formatting converts the exact binary64 value to a decimal integer using
base-10^9 limbs, then rounds to six significant digits with ties to even and
sticky digits. It emits the C locale's default `%g` notation, including signed
zero, infinity, NaN, exponent thresholds, and exponent padding. This deliberately
uses exact arithmetic rather than relying on the host floating-point state.
The generic formatter supports nil, booleans, tagged integers, strings, float
boxes, and opaque pointer/function text. It does not introduce bigint,
bigfloat, complex, or collection runtime support.

The private payload also clones checked static typed-buffer templates and
classifies the bare value subset used by dynamic formatting. Native array data
keeps its public symbol at the first element; a separate private header range
ensures executable writers retain the 32-byte typed-buffer header preceding
that symbol.

Regenerate with `./make bare-payload`, or use `--check` to compare with the
checked-in files. The current files were generated with
Clang/LLD 23.1.1 and llvm-objcopy. The generator rejects absolute/page-relative
relocations, unresolved symbols, and mutable data. In particular, AArch64 uses
the tiny code model so embedding at an arbitrary instruction boundary does not
invalidate ADRP page calculations.

`etc/tests/code/intermediate/ny/bare/runtime-ops.nshape` compares the full
operation matrix with VM output on both architectures with `CC=/nonexistent`.
`asm-artifacts.nshape` retains artifact and unsupported-target checks. The
standalone `runtime-payload.c` regression in the same directory executes the
actual embedded code at a non-page-aligned address, checks headers, integer
extremes, UTF-8 recovery, builder growth/self-aliasing, and compares float output
to `snprintf("%g")`. Build it with `cc -O2 -Isrc .../runtime-payload.c -o test`
and run `test 1000000`. Cross-build with `aarch64-linux-gnu-gcc` and run with
`qemu-aarch64 -L /usr/aarch64-linux-gnu test 100000`.
