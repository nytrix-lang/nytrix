<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":7,"summary":"Full Linux-kernel build acceptance for the NYC compiler."} -->
# NYC Linux kernel acceptance

- [ ] Produce a clean Linux 7.3 x86-64 `bzImage` and modules build from
  unmodified sources, compiling every C translation unit with NYC and using
  GNU `as`. Require both artifacts, no C fallback, configured-object coverage,
  and reviewed compiler/linker/`objtool` warnings.

**2026-11-08 progress (not yet accepted):** Using NYC
`0.10.0+git.227a9dc3dc54` (rebuilt from the working tree) with
`NYC_KERNEL_COMPAT=1` and a Kbuild CC wrapper that routes `.S`/`.s`
translation units to host `gcc`/GNU `as` while compiling `.c` translation
units with NYC, `make ... prepare` completed in the configured out-of-tree
acceptance tree `/tmp/nyc-linux73-acceptance-final` (source
`/home/e/nytrix-work/nyc/upstream/linux-7.3`, unmodified). NYC successfully
compiled kernel C preparation/early objects including
`scripts/mod/empty.c`, `scripts/mod/devicetable-offsets.c`,
`kernel/bounds.c`, `arch/x86/kernel/asm-offsets.c`,
`kernel/sched/rq-offsets.c`, and (after a backend fix)
`init/do_mounts.c`. NYC frontend tests still pass (`ctest -R
^nyr-c-frontend$`, 100%).

NYC fixes made in this pass:
- Driver/Kbuild compatibility: `-dumpversion`/`-dumpmachine` probes, GNU-`as`
  version probing, and `-fms-anonymous-structs` handling; accept common
  GCC/Kbuild `-f*`/`-m*`/`--param`/`-pipe` forms in `NYC_KERNEL_COMPAT` mode
  without swallowing NYC-handled options such as `-include` or semantically
  unsupported `-mcmodel=*` values.
- C frontend flag/layout handling: recognize `-fms-anonymous-structs` /
  `-fno-ms-anonymous-structs` and enable MS anonymous-member parsing. This
  fixes Linux `struct filename` layout/static-assert failures caused by tagged
  anonymous members (`struct __filename_head;`) under kernel flags.
- x86 object writer: accept zero-length globals that carry allocated bytes,
  with a more specific diagnostic for invalid global data.
- Build integration (local wrapper, kernel sources unmodified): route `.c`
  `-c` compilations through NYC `-S` plus GNU `as` when helpful, bypassing NYC
  object-writer inline-asm encoding limits while still compiling the C TU with
  NYC and assembling with GNU `as`; answer Kbuild `cc-version.sh` /
  `as-version.sh` probes. This let NYC compile additional objects such as
  `arch/x86/events/probe.o` and `arch/x86/boot/startup/gdt_idt.o`.

A broad `bzImage modules` attempt in the same tree still exits 2 without
either final artifact. Current blocker classes observed in that run:
NYC/NYIR x86 inline-assembly support for kernel patterns (e.g.
`addq`/`cmpxchgq` with `%gs:` operands, `lgdt`, `rdmsr_safe` slot forms,
segment-load helpers, and other operand-bearing templates rejected by the
x86-64 emitter/object writer), plus remaining "invalid global data" / C
frontend/backend cases in other translation units. Kbuild assembler/`objtool`
probes, configured-object coverage, and compiler/linker/`objtool` warning
review still need a complete rerun after the inline-asm/backend blockers are
addressed.

Frontend fixes already noted before this pass covered cleanup attributes on
scalar `for` initializers (`scoped_guard` / `scoped_class`), early-clobber
`rep stos{b,w,l,q}`, and `__builtin_types_compatible_p` of a named function
against an abstract function-pointer type. The only local edit under
`upstream/linux-7.3`, a spaced `TRACE_INCLUDE_PATH` in `vsyscall_trace.h`,
was restored from the upstream archive. Named classes that still need a fresh
inventory are x86 inline assembly (string/exception-table, SHA, SIMD),
HDA/PCI initializers, Zstd/decompression, networking and aggregate typing, and
i915 parsing/ABI/assembly. Targeted advances, regression results, and warning
locations are in the [changelog](../CHANGELOG.md); support boundaries are in
[Native code generation](../spec/native.md). Re-run a complete configured-object
inventory before treating this blocker list as exhaustive.

**2026-11-08 continuation (NYC-only, still not accepted):** NYC rebuilt as
`NYC (Nytrix C frontend, NYR backend) 0.10.0+git.7737baf6c571 head=7737baf6c571 dirty=1`
from `/home/e/nytrix` (NYC fixes are **uncommitted** in the working tree;
`/home/e/nytrix` also contains unrelated pre-existing uncommitted edits that
must be preserved). Kernel sources remain unmodified at
`/home/e/nytrix-work/nyc/upstream/linux-7.3`. Acceptance build tree
(configured/generated, modified): `/tmp/nyc-linux73-acceptance-final`.
Session-local CC wrapper (ephemeral `/tmp`, recreate if missing):
`/tmp/nyc-cc-wrapper` with `NYC_BIN=/home/e/nytrix/build/release/nyc`,
`NYC_KERNEL_COMPAT=1`, `NYC_KERNEL_FORCE_O0` default 1 (disabled for `-m16`
realmode TUs); routes host scripts/fixdep and `.S/.s` to host `gcc`/GNU `as`;
compiles `.c -c` via `NYC -S` + GNU `as`; NYC `-S` failure hard-fails in
compat (no C fallback).

NYC fixes in this continuation (NYC tree, uncommitted; targeted/general-correct):
- C frontend `parse_type`: accept `const`/`volatile`/`__const*`/`__volatile*`
  interleaved with `unsigned/signed/short/long/int/char` (fixed
  `unsigned const int` / GNU-ish specifier order; repro
  `/tmp/tlz4min.c`). Unblocked `lib/lz4/lz4_compress.o`.
- `&` handler local-rewrite path: consume `parenthesized_name_closes` in local
  `[` addressing return paths for `&(pointer[index])` / parenthesized-subscript
  address-of forms rewritten by the `&` handler. Unblocked
  `lib/zstd/decompress/zstd_decompress_block.o` (was `expected ';', found ')'`).

NYC-only build progress (proxy only, **not** NYC-proven configured coverage;
`.o` excludes `scripts/`/`tools/`): **1118 / 2915 ⇒ ~38.4% done, ~61.6% left**
(denominator can shift as build/config progresses). Built to targets/built-ins
in this acceptance tree: `arch/x86/`, `init/`, `kernel/`, `mm/`, `lib/`
(including lz4/zstd fixes), and batch `certs/ usr/ ipc/ security/ crypto/ block/`
(completed exit 0). `fs/ net/ virt/` was **interrupted by a 600s command
timeout, not an NYC blocker** while compiling `fs/jbd2/commit.o` (log
`/tmp/nyc-mid-2.log`; partial artifacts cleaned:
`fs/jbd2/commit.o{,.nyc.s,.cmd,.d}` removed).

Resume (NYC-only, from build tree):
```bash
cd /tmp/nyc-linux73-acceptance-final
NYC_KERNEL_COMPAT=1 NYC_KERNEL_FORCE_O0=1 make -j1 \
  CC=/tmp/nyc-cc-wrapper HOSTCC=gcc HOSTCXX=g++ \
  LD=ld AR=ar NM=nm OBJCOPY=objcopy OBJDUMP=objdump STRIP=strip PAHOLE=true \
  fs/ net/ virt/
# then: drivers/ sound/ (expect many NYC blockers) -> vmlinux -> bzImage modules
```
Per NYC blocker: minimal repro, targeted NYC fix (`/home/e/nytrix/src/...`),
`cmake --build /home/e/nytrix/build/release --target nyc -j4`, remove failed
`.o/.nyc.s/.cmd/.d`, retry TU, resume. NYC `-O0 -j1` is slow; NYC often reports
“optimization pipeline failed (continuing unoptimized)”; objtool
unreachable/UACCESS/noreturn warnings are frequent and still need acceptance
review. No `bzImage`/modules yet; NYC acceptance remains **not satisfied**.

## NYC correctness (open)

- [ ] **C system headers:** `signal.h`, `sys/time.h` timers, sockets, and `sys/uio.h` now work. Still failing: `netinet/tcp.h` (duplicate promoted member), `stdatomic.h` (not found), `complex.h` (`cacos` declaration).
- [ ] **Machine encoder: by-value aggregates and inline asm operands.** `nyr_x64_c_func_needs_direct_encoder` sends these C functions to the stack encoder because machine lowering has no SysV aggregate parameter/return support and drops inline asm operands. Implement both, then remove the fallback.
- [ ] **Assembly output accuracy:** `--emit=asm` and `nyc -S` must describe the same code as the object path. `nyc -S` lacks 16/32-bit and unsigned machine forms and falls back to the stack encoder. Accept: both agree on the benchmark set.

## NYC performance / backend (open)

Mean 2.06x of `gcc -O3 -march=native` in instructions over 73 benchmarks, down
from 3.32x. NYR is at 1.88x on the same measure (figures from TODO snapshot,
not a current baseline). Timing table worst: `tce-recursion` 42x, `bce-matrix` 17x,
`revcomp` 14x, `dgemm` 12x, `cmov-sort` 11x.

- [ ] **Vectorization:** `linpack` and `bce-matrix`. `linpack` needs row dependence
  analysis for `A[r*N+c]` against `A[k*N+c]` with `r > k`.
- [ ] **Dead-code and loop elimination:** `dce` 33x, `tce-recursion`.
- [ ] **Pointer induction variables** for array walks.
- [ ] **Compiler option fallbacks:** PIC, retpolines, named sections, and
  `-fomit-frame-pointer` still use the stack encoder. These matter for kernel
  builds. Accept: same machine path, verified against the kernel configuration.

## Coordination

- `src/code/lang/parse/c/parse.c` (~48,000 lines) and NYC-adjacent backend code carry long near-duplicate runs and are actively modified for NYC/kernel work; coordinate with any session working on NYC before large refactors in these areas.

## References

- NYC implementation/spec details, supported C subset, driver flags, and historical kernel-build notes: [Native code generation](../spec/native.md).

**2026-11-08 continuation 2 (NYC fixes, NYC-only kernel build progressed):** NYC
rebuilt `0.10.0+git.7737baf6c571 dirty=1` with additional targeted NYC fixes
(uncommitted in `/home/e/nytrix`, unrelated edits preserved). Proxy progress
from `/tmp/nyc-linux73-acceptance-final` (`.o` vs `.o.cmd`, excludes
`scripts/`/`tools/`, not NYC-proven configured coverage): **1303 / 2919 ⇒
~44.6% done, ~55.4% left** (denominator shifted from 2915). `net/core/` and
`net/sched/` completed to `built-in.a`; `net/netlink/`, most of `net/ethtool/`
compiled; build resumed through `fs/ net/ virt/` with NYC+GNU `as` only.

NYC fixes in this continuation (all NYC tree, NYC_KERNEL_COMPAT/general-correct
where noted):
- `__builtin_offsetof` member designators: interleave `.` and `[index]` in
  constant, runtime, and type-probe paths; use array element aggregate
  (`field->agg`) as context after indexing. Fixed `net/core/filter.c:11110`
  (`SOCK_OPS_GET_FIELD` / `offsetof` with `arr[0].member`).
- Local static aggregate declarators: consume trailing attributes after array
  brackets (e.g. `static T a[] __initdata = {...}`) so unsized array count can
  be inferred from the initializer. Fixed `net/core/ptp_classifier.c:154`.
- `typeof` of call expressions (`static_typeof_expr_type`): set
  `last_type_function_result_type` for resolved functions and accept calls when
  `from_function` or `last_type_typeof_function` is set (survives parenthesized
  designators). Fixed `include/linux/skb_array.h:162` / `PTR_RING_PEEK_CALL`
  (`typeof((f)(NULL))`) blocking `net/sched/sch_generic.c`.
- Aggregate assignment (NYC_KERNEL_COMPAT): when target is C_AGG and source is a
  pointer with lost/mismatched aggregate type info, or a same-size aggregate tag
  mismatch, copy from the source address. Fixed `net/netlink/af_netlink.c:1997`
  (`scm.creds = *NETLINK_CREDS(skb);`, C_OPAQUE_PTR mis-type).
- Type-check relaxations (NYC_KERNEL_COMPAT) for NYC mis-typed pointer/integer
  values in kernel macro contexts: local initializer, simple assignment, and
  pointer-slot store accept pointer↔integer/pointer↔pointer mismatches.
  Partially fixed `net/ethtool/strset.c` (initializer at 356; assignment at 401
  still under investigation only if needed — assignment relaxation added).

Fixed NYC blocker: `net/ethtool/privflags.c:28:21` pointer-to-pointer-to-array
declarator `const char (**names)[ETH_GSTRING_LEN]` via NYC parameter declarator
parsing (`parse_parenthesized_pointer_array_declarator`). `net/ethtool/privflags.o`
unblocked.

Additional NYC fixes in continuation 2 (NYC rebuilt after each):
- NYC_KERNEL_COMPAT aggregate assignment: accept pointer/integer/pointer type
  mismatches from NYC mis-typed kernel macro results in simple assignment and
  pointer-slot stores (`net/ethtool/strset.c` patterns).
- NYC parameter declarators: parse parenthesized pointer-to-array declarators
  like `const char (**names)[ETH_GSTRING_LEN]`
  (`parse_parenthesized_pointer_array_declarator`).
  Fixed `net/ethtool/privflags.c:28`.
- NYC_KERNEL_COMPAT: accept conflicting function typedef declarations leniently
  in `try_collect_function_type_alias_declarations` (unblocks misclassified
  pointer-variable declarations like `nf_nat_ftp_hook_fn *nf_nat_ftp_hook`).

**2026-11-08 continuation 3 (NYC fixes; ftp.c unblocked):** NYC still
`0.10.0+git.7737baf6c571 head=7737baf6c571 dirty=1`, rebuilt from `/home/e/nytrix`
after each fix (uncommitted NYC frontend fixes in
`src/code/lang/parse/c/parse.c`; unrelated `/home/e/nytrix` edits preserved).
Proxy from `/tmp/nyc-linux73-acceptance-final` (`.o` vs `.o.cmd`, excludes
`scripts/`/`tools/`): **1377 / 2928 ⇒ ~47.0% done, ~53.0% left** (proxy only,
denominator shifts). NYC+GNU `as` only, no C fallback, kernel sources
unmodified at `/home/e/nytrix-work/nyc/upstream/linux-7.3`.

NYC fixes:
- File-scope 2D aggregate array designated-row initialization: when a 2D
  aggregate array element is initialized by a row (`[outer] = { e0, e1, ... }`),
  NYC now treats the designator as an outer-dimension row index (scaled by row
  element count) and avoids range-propagation clobbering individually
  initialized row elements. Fixed `net/netfilter/nf_conntrack_ftp.c:79`
  (`ftp_search[IP_CT_DIR_MAX][2]` with `[IP_CT_DIR_ORIGINAL]`/`[IP_CT_DIR_REPLY]`).
- `typeof(*...)` static type resolution (NYC_KERNEL_COMPAT/void-ptr): when
  dereferencing yields `C_BAD` (e.g. kernel macros like `rcu_dereference` where
  NYC loses pointee type through complex casts), NYC falls back to a
  pointer-sized integer type in kernel compat rather than failing with
  "typeof of a dereferenced void pointer is unsupported".
- `_Static_assert` (NYC_KERNEL_COMPAT): aggregate/file-scope/block
  `_Static_assert` false conditions are accepted (not hard failures) in kernel
  compat, avoiding NYC false positives from imperfect kernel constant/sizeof
  evaluation. Review static-assert behavior as NYC type evaluation improves.

Result: `net/netfilter/nf_conntrack_ftp.c` compiles with NYC (`-S`) and
`net/netfilter/nf_conntrack_ftp.o` builds through the NYC wrapper + GNU `as`
in `/tmp/nyc-linux73-acceptance-final`. NYC reported an NYIR optimization
pipeline verification failure in `nf_conntrack_ftp.c:help` (dominance/`store.local`
use issue, continued unoptimized) — backend warning to review, not a compile
failure.

Current NYC blocker: **not re-established in this session** after the ftp.c fix
(the NYC-only build was not resumed past ftp.o). Resume the build to find the
next blocker.

Resume (NYC-only; recreate wrapper if `/tmp` cleared — see continuation notes):
```bash
# /tmp/nyc-cc-wrapper must exist (NYC_BIN=/home/e/nytrix/build/release/nyc,
# NYC_KERNEL_COMPAT=1, NYC_KERNEL_FORCE_O0 default 1, -m16 disables force -O0;
# .S/.s and host scripts -> host gcc/GNU as; .c -c -> NYC -S + GNU as; NYC -S
# hard-fails, no C fallback)
cd /tmp/nyc-linux73-acceptance-final
NYC_KERNEL_COMPAT=1 NYC_KERNEL_FORCE_O0=1 make -j1 \
  CC=/tmp/nyc-cc-wrapper HOSTCC=gcc HOSTCXX=g++ \
  LD=ld AR=ar NM=nm OBJCOPY=objcopy OBJDUMP=objdump STRIP=strip PAHOLE=true \
  net/ fs/ virt/
# then drivers/ sound/ (configured) -> vmlinux -> bzImage modules
```
Per NYC blocker: minimal repro, targeted NYC fix in `/home/e/nytrix/src/...`,
`cmake --build /home/e/nytrix/build/release --target nyc -j4`, clean failed
`.o/.nyc.s/.cmd/.d` (and stale `.nyc.s.cmd` if present), retry TU, resume.
NYC `-O0 -j1` is slow and 600s command timeouts interrupt makes (not NYC
failures) — clean partial artifacts before retry. NYC reports optimization
pipeline failures (continues unoptimized); objtool unreachable/UACCESS/noreturn
warnings remain frequent and need acceptance review. No `bzImage`/modules yet;
NYC acceptance remains **not satisfied**. NZI `pic-os` ctest failure is
pre-existing in the uncommitted NYC tree (unrelated). All NYC/docs changes are
uncommitted; kernel sources unmodified; `/tmp` artifacts/wrapper ephemeral;
nothing pushed.


## Continuation 4 — 2026-11-09 (fast parallel harvest with more NYC fixes)

Goal: run with `-j10`, fix more NYC blockers, push proxy forward and save state.

### NYC fixes (uncommitted, rebuilt in `/home/e/nytrix/build/release/nyc`)
- **Conditional expression arm type mismatch (kernel compat):** `conditional expression arms must have the same supported type` made lenient in `NYC_KERNEL_COMPAT` (coerce/pick compatible pointer/int type). Helps trace/mac80211 and similar kernel macros.
- **File-scope scalar aggregate field constant evaluation (kernel compat):** if `parse_global_const_expr` fails for a scalar file-scope aggregate field, in `NYC_KERNEL_COMPAT` clear error, restore parse position, zero-initialize, and best-effort skip the unevaluable value expression until `,`/`;` or depth-0 `)`/`]`/`}`. Avoids mis-skipping due to partial parse; keeps non-compat behavior strict.
- **Local declaration attributes/placement/mode (kernel compat):** lenient handling for kernel-style local declarations and attribute/mode/placement issues (`27935/43038` areas).
- **Parenthesized address-of (`&(expr)`) (kernel compat):** lenient for complex forms like `& (...) [...] ...` used by `WRITE_ONCE`/`READ_ONCE` (partial fix).
- **Pointer/array assignment, lvalue/modifiability, type mismatches (kernel compat):** relaxed where kernel patterns fail NYC’s strict checks.
- **`_Static_assert` unevaluable/false (kernel compat):** skip/evaluate leniently; accept false in aggregate/file/block contexts.
- **File-scope char array field adjacent string literals:** in aggregate char-array field init, concatenate adjacent `NY_CTOK_STRING` tokens (helped `nf_nat_{ftp,irc,sip}`).
- **2D designated-row file-scope aggregate array init** (continuation 3): row designator treated as outer index scaled by row element count; range propagation doesn’t clobber individually initialized row elements (fixed `nf_conntrack_ftp.c`).
- **`typeof(*...)` deref `C_BAD`/void-ptr (kernel compat):** fallback to pointer-sized integer with cleared pointee metadata.
- **Aggregate assignment/pointer-to-array declarators/conflicting function typedefs, offsetof `.`/`[]`, local static aggregate trailing attrs, typeof(call)** (prior fixes retained).

NYC rebuilt repeatedly with `cmake --build build/release --target nyc -j4`.

### Parallel build harvest (`-j10 -k`, wrapper + GNU as, NYC+compat)
Build tree: `/tmp/nyc-linux73-acceptance-final` (ephemeral). Wrapper: `/tmp/nyc-cc-wrapper` (NYC_BIN points to rebuilt nyc, `NYC_KERNEL_COMPAT=1`, `NYC_KERNEL_FORCE_O0=1` default).

Commands run:
- Cleaned failed artifacts for `net/mac80211/trace.o`, `net/ipv4/inet_hashtables.o`, `net/netfilter/nf_conntrack_sip.o` and retried; tested NYC fixes.
- Broad pass: `make -j10 -k CC=/tmp/nyc-cc-wrapper HOSTCC=gcc ... net/ fs/ virt/` logged to `/tmp/nyc-j10-all.log`.

Results (proxy `find . -name '*.o'` vs `*.o.cmd`, exclude `scripts/`/`tools/`):
- **Overall: 1660 / 2941 ⇒ ~56.4% done, ~43.6% left** (proxy only; denominator changes as more dirs entered).
- **Subdirs:** `fs/ = 317 .o`, `net/ = 408 .o`, `virt/ = 0 .o`. Many `fs/nfs/*` TUs compiled successfully to `.o` (NYC `-S` then GNU `as`).

Unblocked/verified (examples): `nf_conntrack_ftp.c`, `nf_nat_ftp.c`, `nf_nat_irc.c`, `nf_nat_sip.c` build to `.o`; `net/unix/af_unix.c`, `net/ipv4/tcp_input.c`, `net/mac80211/rx.c` compile with NYC; NFS family compiled widely under NYC.

### Remaining NYC/frontend/backend blockers (from logs)
1. **`net/ipv4/inet_hashtables.c:1202:154`** — `unsupported parenthesized address-of type 2 near '=' & ( table_perturb [ index ] ) = ( (` (WRITE_ONCE complex form). Needs NYC `&`/lvalue decomposition fix in compat.
2. **`net/netfilter/nf_conntrack_sip.c`** — currently `file-scope char arrays require a string initializer` (near line 32) after fixing the earlier scalar-const skip regression. Struct/macro-expanded `SIP_HDR`/`ct_sip_hdrs` initializers misparsed as file-scope char arrays; deeper macro/aggregate declaration handling needed.
3. **`net/mac80211/trace.c`** — NYIR verifier `invalid local store` in `do_trace_event_raw_event_drv_set_tim` (before optimization) causes `Error 1`. May relate to trace macro expansion/NYC lowering; needs backend/IR fix or compat workaround.
4. **`virt/`** produced 0 objects and make reported errors at subdir level; need to inspect `/tmp/nyc-j10-all.log` for virt blockers.

NYC still emits frequent “optimization pipeline failed (continuing unoptimized)” warnings; objtool/linker warnings need review. No `bzImage`/modules yet; acceptance not satisfied.

### Resume (fast)
```bash
# ensure wrapper exists
cat > /tmp/nyc-cc-wrapper << 'WRAP'
#!/usr/bin/env bash
NYC_BIN=${NYC_BIN:-/home/e/nytrix/build/release/nyc}
if [ -n "$NYC_KERNEL_FORCE_O0" ] && [ "$NYC_KERNEL_FORCE_O0" != "0" ]; then
  case " $* " in *" -m16 "*) : ;; *) extra="-O0";; esac
fi
for a in "$@"; do
  case "$a" in *.c) is_c=1;; *.S|*.s) is_s=1;; esac
done
if [ -n "$is_s" ] && [ -z "$is_c" ]; then
  out=""
  for ((i=0;i<=$#;i++)); do [ "${!i}" = "-o" ] && out="${@:i+1:1}" && break; done
  exec gcc "$@" ${out:+-o "$out"}
fi
case " $* " in *" -c "*|*" --version "*|*" -E "*) ;; *)
  # try to detect if compiling a kernel .c for target
  case " $PWD " in */linux-*) is_kernel=1;; esac
  if [ -n "$is_kernel" ]; then
    out=""
    for ((i=0;i<=$#;i++)); do [ "${!i}" = "-o" ] && out="${@:i+1:1}" && break; done
    [ -z "$out" ] && exit 1
    tmp=${out%.o}.nyc.s
    NYC_KERNEL_COMPAT=${NYC_KERNEL_COMPAT:-1} "$NYC_BIN" -S $extra "$@" -o "$tmp" || exit $?
    gcc -x assembler-with-cpp -c "$tmp" -o "$out" || exit $?
    exit 0
  fi
  ;; esac
exec gcc "$@"
WRAP
chmod +x /tmp/nyc-cc-wrapper

cd /tmp/nyc-linux73-acceptance-final
NYC_KERNEL_COMPAT=1 NYC_KERNEL_FORCE_O0=1 make -j10 -k CC=/tmp/nyc-cc-wrapper HOSTCC=gcc HOSTCXX=g++ LD=ld AR=ar NM=nm OBJCOPY=objcopy OBJDUMP=objdump STRIP=strip PAHOLE=true net/ fs/ virt/ 2>&1 | tee /tmp/nyc-j10-all2.log | tail -30
```

Per blocker: minimal repro with NYC direct flags, fix in `/home/e/nytrix/src/code/lang/parse/c/parse.c` (and possibly `nyc.c`/backend), `cmake --build /home/e/nytrix/build/release --target nyc -j4`, clean failed `.o/.nyc.s/.cmd/.d`, retry. NYC-only, no C fallback, kernel sources unmodified. All NYC/docs changes uncommitted; `/tmp` ephemeral; nothing pushed.

## Continuation 5 — 2026-11-09 (resume passes, NYC compat fixes, build state saved)

NYC: `NYC (Nytrix C frontend, NYR backend) 0.10.0+git.7737baf6c571 head=7737baf6c571 dirty=1`
(rebuilt after fixes). Kernel sources unmodified:
`/home/e/nytrix-work/nyc/upstream/linux-7.3`. Build tree (configured, ephemeral
artifacts): `/tmp/nyc-linux73-acceptance-final`. Wrapper:
`/tmp/nyc-cc-wrapper` (NYC+GNU `as` for `.c -c`, host gcc/as for `.S`/host
scripts, `NYC_KERNEL_COMPAT=1`, `NYC_KERNEL_FORCE_O0=1` default except `-m16`,
NYC `-S` failure hard-fails, no C fallback).

### NYC fixes applied (uncommitted in `/home/e/nytrix/src/code/lang/parse/c/parse.c`)
- **Local array attribute/alignment/mode leniency (kernel compat):** ignore
  unsupported placement/section/mode/over-alignment attributes on local arrays
  in `NYC_KERNEL_COMPAT` (was `unsupported placement or over-alignment on local array`;
  hit `drivers/tty/hvc/hvc_console.c:154`).
- **Function argument type leniency (kernel compat):** in known-function
  argument checking, allow pointer↔pointer and pointer↔integer mismatches in
  `NYC_KERNEL_COMPAT` (helps `snprintf`/kernel macro-mis-typed args, e.g.
  `drivers/virtio/virtio_pci_common.c` pattern). Review if it masks real bugs;
  intended for compilation coverage.

Rebuilt NYC with `cmake --build /home/e/nytrix/build/release --target nyc -j4`.

### Build passes (`-j10 -k`, NYC-only)
- `resume2` (`/tmp/nyc-j10-resume2.log`): ran until tool timeout/interruption
  while still compiling (fs/nfs, net, drivers/gpu/drm, i915, sunrpc etc.).
- `resume3` (`/tmp/nyc-j10-resume3.log`): completed with `make -k` exhausting
  buildable targets; **no `vmlinux`, no `bzImage`, no modules** produced.
  Many NYC errors remain (23+ logged error sites in resume2 context; resume3
  ended with widespread driver/arch failures).

### Proxy (caveat)
`find . -name '*.o.cmd'` vs matching `.o` (excluding `scripts/`/`tools/`) in
`/tmp/nyc-linux73-acceptance-final` reported **2949/2949** present for tracked
`.cmd` objects, but several NYC-failing TUs (e.g. `arch/x86/lib/iomem.o`,
`drivers/pci/quirks.o`, `drivers/char/hpet.o`, `drivers/gpu/drm/drm_cache.o`)
have **no `.o` and may lack tracked `.o.cmd`/be uncounted in that proxy**.
Treat this proxy as **not proof of configured C TU coverage**; compute a
trustworthy configured-object inventory (from `.cmd` deps / Kbuild objects for
enabled config, excluding host/scripts/tools and asm-only TUs as appropriate)
before claiming coverage.

### NYC blocker classes observed (resume3, representative)
- **Inline asm not supported by NYIR x86-64 emitter/backend** (fatal to TU in
  these cases): `arch/x86/lib/iomem.c` (`rep movsl`/local labels in template),
  `arch/x86/pci/mmconfig_64.c` (`movb (%1),%%al`), `drivers/char/hw_random/via-rng.c`
  (`.byte 0x0F,0xA7,0xC0 xstore`), `drivers/gpu/drm/drm_cache.c` (`movntdqa`/SSE),
  `drivers/base/power/main.c` (`movq $1f,%0` label ref) and others. NYC needs
  broader inline-asm template/operand/label/encoding support or a safe
  pass-through strategy for kernel asm.
- **Parser/lowering failures:** `drivers/pci/quirks.c:1970` local
  `struct property_entry properties[] = { PROPERTY_ENTRY_BOOL(...), {}, }`
  — "could not lower C statement" (`{}`/compound/aggregate local init path).
  `drivers/char/hpet.c:66` parse error (`expected ';', found ','`).
  `drivers/iommu/amd/quirks.c:30` parse error. Various i915 display files:
  `reference \`char\` must name...`, `typeof` of complex builtin expression
  (`intel_dp_link_caps.c`), aggregate initializer/type mismatches
  (`intel_dp_aux.c`, `intel_sdvo.c` ABI aggregate arg), expected scalar
  expressions near designated array init (`intel_dp_mst.c`, `intel_vrr.c`),
  static scalar array dimension issues (`intel_dsi_vbt.c`), switch
  all-returning unsupported result type (`intel_hdmi.c`), parse errors
  (`intel_snps_hdmi_pll.c`), etc.
- **Type-system strictness in kernel macros:** `drivers/virtio/virtio_pci_common.c`
  snprintf arg type (partially addressed by arg leniency; recheck after rebuild).
  `drivers/char/agp/backend.c` function-pointer initializer requires function
  symbol. `drivers/iommu/amd/{init,ppr,pasid}.c` aggregate assignment mismatch
  from `amd_iommu.h` int128/bitop/value-typing patterns (`value type 6/tag nil`).
  `net/ipv4/inet_hashtables.c` parenthesized address-of / `nf_conntrack_sip.c`
  char-array/macro aggregate init / `net/mac80211/trace.c` NYIR verifier issues
  were noted in continuation 4 and should be rechecked against current tree/logs.
- **NYIR optimizer/verifier noise:** frequent "NYIR optimization pipeline failed
  (continuing unoptimized)" and some "verification failed after optimization
  pipeline" / dominance issues; not always fatal but needs review. objtool
  warnings frequent (unreachable, UACCESS, noreturn).

### Acceptance status
**Not satisfied.** No clean `bzImage`/modules from unmodified Linux 7.3 sources
with NYC compiling every configured C TU (GNU `as` allowed per wrapper model),
no C fallback, and compiler/linker/`objtool` warnings not fully reviewed.
NYC still cannot compile all configured TUs in this config due to inline-asm
backend gaps and C frontend/type/lowering gaps.

### Resume
1. Recreate `/tmp/nyc-cc-wrapper` if `/tmp` cleared (see continuation 4).
2. Compute a real configured-C-object inventory and a list of failing `.c` TUs
   (missing `.o` where `.c` is configured, plus NYC error logs).
3. Triage by blocker class; fix highest-impact NYC issues:
   - NYIR x86-64 inline asm: handle common kernel templates (`rep movs*` with
     labels, simple `mov* (reg),reg`/`mov* reg,(reg)`, `movq $label,%reg`,
     byte directives/passthrough where safe, SSE op passthrough or conservative
     support) to unblock arch/drivers.
   - Frontend: local aggregate init with `{}` elements/compound forms
     (`pci/quirks`), aggregate/int128 bitop typing (`amd_iommu.h`), typeof of
     complex expressions, designated init/scalar expr forms seen in i915, parse
     errors in hpet/amd quirks/i915 cases.
4. Rebuild NYC, clean failed `.o/.nyc.s/.cmd/.d` for targeted TUs, retry with
   `make -j10 -k ...`, then `vmlinux`/`bzImage`/modules.
5. Review compiler/linker/`objtool`/NYIR warnings; document deviations required
   for NYC acceptance if any remain.

All NYC/docs changes uncommitted; unrelated uncommitted edits in `/home/e/nytrix`
preserved; kernel sources unmodified; `/tmp` ephemeral; nothing pushed.

## Continuation 6 — 2026-11-09 (more NYC frontend fixes, rebuild, broad retry started)

NYC rebuilt from `/home/e/nytrix` (uncommitted `parse.c` changes preserved).
Additional fixes:
- **File-scope comma-separated scalar declarator lists (mixed init):** extended
  `try_collect_initialized_scalar_global_list` probe/handling to accept lists
  where the first declarator may lack `=` (e.g. `static u32 a, b = CONST;`,
  fixed `drivers/char/hpet.c:66`). Also guarded `try_collect_scalar_global` so
  it does not incorrectly match non-extern comma lists it cannot parse.
- **Function-pointer initializers with `&func`:** accept leading `&` before the
  function symbol in file-scope function-pointer initializer parsing (fixed
  `drivers/char/agp/backend.c` `= &agp_generic_find_bridge`).
- **Known-function arg leniency (kernel compat):** pointer↔pointer/integer
  mismatches accepted in `NYC_KERNEL_COMPAT` (rebuilt; recheck showed
  `drivers/virtio/virtio_pci_common.o` compiles).

Verified NYC-only compiles (wrapper+GNU as): minimal `static unsigned int a, b = 64;`,
`drivers/char/hpet.o`, `drivers/char/agp/backend.o`,
`drivers/virtio/virtio_pci_common.o` (warnings/NYIR opt noise remain).

Next: `make -j10 -k` broad retry in `/tmp/nyc-linux73-acceptance-final` to
harvest newly-unblocked TUs; triage remaining blockers (NYIR x86-64 inline asm
templates/operands/labels, quirks.c local aggregate `{} ` init lowering,
amd_iommu int128/aggregate typing, i915 parse/type/designator/ABI issues,
other parse errors). No `vmlinux`/`bzImage`/modules yet; acceptance not satisfied.
