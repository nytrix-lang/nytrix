<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"spec","order":46,"summary":"Link request fields, tool selection, and the in-process ELF executable linker."} -->
# Linking

The [Nyr C API](nyr.md) links object files and static archives through an
in-process ELF linker or external LLVM tools. Source compilation and the
compiler driver's application-link path are separate.

Declarations are in [`<nyr/nyr.h>`](../../src/include/nyr/nyr.h);
dispatch and ELF linking are implemented in
[`linker.c`](../../src/code/backend/object/linker.c).

## Link requests

`nyr_link(request, err, err_len)` accepts a `nyr_link_request_t`:

| Field | Meaning |
| --- | --- |
| `input_paths`, `input_count` | Nonempty array of object or archive paths. Runtime and library files can be supplied as inputs. |
| `output_path` | Required output file path. |
| `target_triple` | Explicit target triple, or `NULL` for the host default. |
| `entry_symbol` | Optional executable entry symbol; its calling convention depends on the selected path. |
| `kind` | `NYR_LINK_EXECUTABLE`, `NYR_LINK_SHARED_LIBRARY`, or `NYR_LINK_STATIC_ARCHIVE`. |

The call returns `true` on success and clears a supplied nonempty error
buffer. On failure it returns `false` and writes a diagnostic when a buffer is
available. Paths and request storage remain caller-owned for the call.

### Path selection

| Request | Implementation |
| --- | --- |
| Linux x86-64 host executable, `target_triple == NULL`, and an entry symbol | In-process ELF linker. |
| Static archive output | `llvm-ar rcs`. |
| Wasm executable | `wasm-ld`; uses `--no-entry` when no entry is supplied. |
| Other executable or shared-library output | `clang -fuse-ld=lld`, with target and output-kind options. |
| Wasm shared library | Rejected. |

An explicit target triple selects the external path even if it names the
host. The API launches tools directly, without a shell. `NYR_LINK_CLANG`,
`NYR_LINK_WASM_LD`, and `NYR_LINK_AR` override their executable paths.
Cross-target links still require compatible tools, input objects, and any
necessary target runtime or sysroot.

For external executable links, `entry_symbol` names the platform entry rather
than a C `main` function. Mach-O requests add the platform's leading underscore.
An explicit entry or shared-library request omits normal startup libraries
through `-nostdlib`; callers supply required libraries as inputs. The request
structure has no general-purpose linker-argument or library-search-path field.

## In-process ELF executables

`nyr_link_elf64_executable` links one ELF64 x86-64 relocatable object.
`nyr_link_elf64_executable_objects` accepts several objects and static archives.
These entry points are available on Linux x86-64 and do not start an external
linker.

The entry function takes no arguments and returns an `i64`. The linker adds a
startup stub that calls it and uses the low byte of its result as the process
exit status. This is not the platform's raw process-entry convention.

The linker resolves global and weak definitions across selected inputs and
lazily extracts archive members needed by unresolved strong references. It
parses selected members from the loaded archive buffer, without temporary
object files. Duplicate strong definitions, unresolved strong symbols,
unsupported relocations, writable executable sections, and unsupported object
formats are errors.

This path produces a static executable. It does not provide shared-library
loading, ordinary C startup, or the compiler driver's full runtime and link
options. Use the external request path or the compiler driver when those are
needed.

### Request example

This C fragment requires an existing ELF64 x86-64 object defining
`int64_t answer(void)`. Run the resulting executable on Linux x86-64:

```c
#include <nyr/nyr.h>

const char *inputs[] = {"answer.o"};
nyr_link_request_t request = {
    .input_paths = inputs,
    .input_count = 1,
    .output_path = "answer",
    .target_triple = NULL,
    .entry_symbol = "answer",
    .kind = NYR_LINK_EXECUTABLE,
};
char error[256] = {0};
bool linked = nyr_link(&request, error, sizeof(error));
```

## Coverage

The [public API acceptance program](../../src/code/backend/ir/test/public_acceptance.c)
executes linked outputs and checks multiple objects, archive extraction,
strong/weak resolution, duplicate definitions, and unresolved-symbol errors.
Run it after building the acceptance target:

```bash
cmake --build build/release --target nyr-backend-ir-acceptance
build/release/nyr-backend-ir-acceptance
```

Object emission and linking have separate capability boundaries. A target's
object writer being available does not establish that the corresponding
external linker, startup objects, or runtime libraries are installed.
