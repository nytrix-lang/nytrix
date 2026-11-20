<!-- nytrix-doc: {"audience":"user","featured":true,"group":"spec","order":45,"summary":"Runtime value representation, allocation prefixes, container slots, and ABI conversion boundaries."} -->
# Memory

Compiler and runtime code share these representation rules. Ordinary Nytrix
programs must use the public APIs instead of depending on internal addresses.

> **Note:** Usually unnecessary unless implementing runtime code or diagnosing
> a representation boundary.

> **Warning:** Never construct internal headers in applications; use the owning API.

## Contents

- [Runtime layouts and values](#runtime-layout-at-a-glance)
- [Container slots](#container-slots)
- [Function and ABI boundaries](#function-and-abi-boundaries)

## Runtime layout at a glance

| Mode | Layout before object body |
| --- | --- |
| Default allocator | 32-byte runtime prefix |
| Tracing GC | 16-byte GC header, then 32-byte runtime prefix |
| Container fields | Representation depends on the slot; some fields are tagged values and others are raw pointers |

## Values

A dynamic `NyValue` uses a tagged representation. A small integer `n` is
stored as `(n << 1) | 1`; the low bit distinguishes it from aligned heap
pointers. `nil`, booleans, heap objects, and native handles have their own
runtime forms. Heap object kinds are recorded near the object by tags such as
`TAG_LIST`, `TAG_DICT`, `TAG_DICT_TBL`, `TAG_STR`, and `TAG_FLOAT`.

```text
tagged int n:      bits: | n<<1 | 1 |        (low bit set, value in the rest)
heap pointer p:    low bits zero (16-byte alignment)   (low bit clear)
```

The low bit is the discriminator: a dynamic value with bit 0 set is a tagged
integer; bit 0 clear means a heap pointer, nil, or a native handle form.

Never apply integer untagging to an arbitrary pointer or object value. A right
shift is valid only when the owning boundary has established that the value is
a tagged integer.

## Allocation and collection

The ordinary allocator returns a 16-byte-aligned object pointer with a 32-byte
runtime prefix immediately before it. The prefix contains the allocation magic
and tagged body size; small allocations may be reused from thread-local pools.
Reference accounting tracks ordinary managed allocations. Tracing GC adds
a 16-byte header before the runtime prefix and moves
supported managed objects.

```text
addresses grow ->

  [ GC header 16B ] [ runtime prefix 32B ] [ body ... ]    (GC mode)
                   [ runtime prefix 32B ] [ body ... ]     (default)

  object pointer returned by the allocator points at the first body byte
```

The runtime header/prefix layout is internal. Code must allocate and release
through its owning runtime or library operation rather than constructing a
header manually.

The native NYIR optimizer can replace a bounded, fixed-size typed-buffer
allocation with function-local stack storage when it proves that the buffer
does not escape and every use stays within the payload. The promoted storage is
zeroed at the original allocation point, including when that point is inside a
loop. Unknown calls, returns, merged pointers, and unproven offsets prevent
promotion. This is a conservative optimization, not a source-level lifetime
guarantee; ordinary values and buffers still follow their selected heap policy.

### Frame collection

Lists, dictionaries and strings have no owner in the language, so a program
that builds them every frame would grow without bound. `__heap_collect(mode)`
is the runtime's answer for programs with a frame loop; `core.os.ui.render`'s
`end_frame` calls it, so UI programs need no change.

| `mode` | Effect |
| --- | --- |
| `0` | Frame boundary. The first call starts recording what the calling thread allocates; later calls collect when enough has accumulated and otherwise free a slice of what the last collection found. |
| `1` | Collect the objects recorded since the last collection now. |
| `2` | Full collection: also reconsider objects that survived earlier ones. |
| `10`-`16` | Statistics: collections, full collections, objects freed, bytes freed, last pause (microseconds), recorded survivors, longest pause (microseconds). |

The result of modes 0-2 is the number of bytes found unreachable.

What is collected: lists, dictionaries, strings (heap and small), byte
buffers, big integers and result objects that the recording thread allocated
after the first call. Objects that predate it, objects made by other threads,
and memory the program obtained with `malloc` are never freed by the
collector.

How reachability is decided: conservatively. Any aligned word in writable
memory that holds an address inside a recorded object keeps it alive, whether
the word is the address itself, the address carried as a tagged integer, or
the native handle encoding. This covers references held in registers, in raw
buffers and in C structures without compiler support. The cost is that a
stray word can keep an object alive; it can never free one that is in use.
A reference that exists only outside the process's ordinary private memory
(in a file, a shared mapping, a device buffer, or packed at an odd byte
offset) is not seen.

A collection reads only the memory written since the previous one. On Linux
6.7 and later the pages are tracked with asynchronous userfaultfd write
protection and `PAGEMAP_SCAN`; older kernels use soft-dirty bits; without
either, all writable memory is read each time and collections are spaced
further apart. Other threads the runtime started are paused for the scan.
With userfaultfd tracking, everything is write-protected when recording
starts, and every recorded object is allocated after that, so a reference
to one can only be in memory written since: a full collection reads those
pages (kept as a set of ranges) and memory that is not tracked, never the
rest of the process. A program run in-process by the JIT therefore does not
read the compiler's gigabytes of heap. While recording, strings, byte
buffers and other managed blocks come from the collector's own pool like
lists and dictionaries, so a frame's garbage stays on few pages.
The kernel's part of a collection still costs time for every resident page
of the process, so a program run in-process by the JIT pauses for tens of
milliseconds where a standalone executable pauses for a few; collections are
spaced so that this part stays below a fortieth of the run time.

The collector exists on Linux with glibc. Elsewhere `__heap_collect` returns
0 and nothing is freed.

| Variable | Meaning |
| --- | --- |
| `NYTRIX_HEAP_COLLECT=0` | Never start the collector. |
| `NYTRIX_HEAP_COLLECT_BYTES=N` | Young bytes that trigger a collection (default 2 MiB). |
| `NYTRIX_HEAP_COLLECT_MAJOR_BYTES=N` | Promoted bytes that trigger a full collection (default 48 MiB). |
| `NYTRIX_HEAP_COLLECT_TRACK=soft-dirty` or `none` | Force the older page-tracking method, or none. |
| `NYTRIX_HEAP_COLLECT_TRACE=1` or `2` | One line per collection on stderr; `2` adds where the scan time went. |

### Frame collection

Lists, dictionaries and strings have no owner in the language, so a program
that builds them every frame would grow without bound. `__heap_collect(mode)`
is the runtime's answer for programs with a frame loop; `core.os.ui.render`'s
`end_frame` calls it, so UI programs need no change.

| `mode` | Effect |
| --- | --- |
| `0` | Frame boundary. The first call starts recording what the calling thread allocates; later calls collect when enough has accumulated and otherwise free a slice of what the last collection found. |
| `1` | Collect the objects recorded since the last collection now. |
| `2` | Full collection: also reconsider objects that survived earlier ones. |
| `10`-`16` | Statistics: collections, full collections, objects freed, bytes freed, last pause (microseconds), recorded survivors, longest pause (microseconds). |

The result of modes 0-2 is the number of bytes found unreachable.

What is collected: lists, dictionaries, strings (heap and small), byte
buffers, big integers and result objects that the recording thread allocated
after the first call. Objects that predate it, objects made by other threads,
and memory the program obtained with `malloc` are never freed by the
collector.

How reachability is decided: conservatively. Any aligned word in writable
memory that holds an address inside a recorded object keeps it alive, whether
the word is the address itself, the address carried as a tagged integer, or
the native handle encoding. This covers references held in registers, in raw
buffers and in C structures without compiler support. The cost is that a
stray word can keep an object alive; it can never free one that is in use.
A reference that exists only outside the process's ordinary private memory
(in a file, a shared mapping, a device buffer, or packed at an odd byte
offset) is not seen.

A collection reads only the memory written since the previous one. On Linux
6.7 and later the pages are tracked with asynchronous userfaultfd write
protection and `PAGEMAP_SCAN`; older kernels use soft-dirty bits; without
either, all writable memory is read each time and collections are spaced
further apart. Other threads the runtime started are paused for the scan.
The kernel's part of a collection costs time for every resident page of the
process, so a program run in-process by the JIT, next to the compiler's heap,
pauses for tens of milliseconds where a standalone executable pauses for a
few; collections are spaced so that this part stays below a fortieth of the
run time.

The collector exists on Linux with glibc. Elsewhere `__heap_collect` returns
0 and nothing is freed.

| Variable | Meaning |
| --- | --- |
| `NYTRIX_HEAP_COLLECT=0` | Never start the collector. |
| `NYTRIX_HEAP_COLLECT_BYTES=N` | Young bytes that trigger a collection (default 2 MiB). |
| `NYTRIX_HEAP_COLLECT_MAJOR_BYTES=N` | Promoted bytes that trigger a full collection (default 48 MiB). |
| `NYTRIX_HEAP_COLLECT_TRACK=soft-dirty` or `none` | Force the older page-tracking method, or none. |
| `NYTRIX_HEAP_COLLECT_TRACE=1` or `2` | One line per collection on stderr; `2` adds where the scan time went. |

## Container slots

Dynamic containers contain both tagged values and raw implementation fields.
For a dictionary, logical count and capacity slots are tagged integer values;
the table slot is a raw heap pointer. A list stores its logical length as a
tagged value and its elements as dynamic values. Untagging a raw table pointer
as if it were capacity corrupts the representation.

A dictionary object with `n` entries uses:

```text
dict object:
  +0x00  count    (tagged integer)
  +0x08  capacity (tagged integer)
  +0x10  table    (raw heap pointer to the entry array)

entry array:
  -0x08  TAG_DICT_TBL (raw tag slot)
  +0x00  slot 0: [state|key]   two 8-byte slots per entry
  +0x10  slot 1: [state|key]
  ...
```

The table pointer is a raw address, not a tagged value. Reading it through a
dynamic-value load applies the tagged-integer untag, which corrupts it. Use
the owning list/dictionary helpers for dynamic containers. Native code must
preserve the exact slot convention established by the runtime layout.

## Function and ABI boundaries

Typed native `i64` values are raw machine integers. Dynamic values remain
tagged. Entering a dynamic boundary boxes a raw integer; leaving one unboxes it
only after an integer check. Function lowering applies the corresponding
conversion at typed/dynamic boundaries, including parameters, returns, and
loads.

Native raw-allocation entries consume an untagged byte count. A typed integer
passes unchanged; an integer carried through `any` is checked and decoded
exactly once. The compiler's standard-library runtime alias and its argument
conversion must select the same ABI, even when generated core declarations
retain a legacy link name. This does not rewrite explicit user C bindings.

| Boundary | Value form | Rule |
| --- | --- | --- |
| Typed `int` parameter/slot | Raw signed 64-bit `i64` | No tag applied. |
| Dynamic `any` parameter | Tagged `NyValue` | Box on entry, check-and-unbox on exit. |
| Typed `int` load result | Raw `i64` | No untag; the slot is raw. |
| Dynamic load result | Tagged `NyValue` | Untag (`>> 1`) only after an integer check. |
| `int`-typed helper returning a pointer | Undefined | Raw pointers are not integers; see below. |

A raw pointer is not an `int`. Pass it through a pointer/handle/`any` boundary
appropriate to the API, never by casting it to an integer slot and relying on
integer retagging. This prevents the class of failures where a pointer is
shifted or retagged as a numeric value during a call or load. When a function
returns a raw implementation pointer, declare the return and parameter types so
the pointer crosses as a heap/`any` value, not as an `int`.

## Debugging representation faults

Start with the narrowest executable oracle. Inspect NYIR and machine-form
output only to identify the owning conversion boundary, then add a focused
native regression that crosses it. Do not use emitted assembly alone as proof
of a correct runtime representation.

A fault pattern that should never appear in native checks:

- a helper declared `fn ... int` returning a heap pointer;
- code that retags the shifted result with `(v << 1) | 1`;
- stores against that retagged base that silently miss the real object.

When those shapes appear, the owning conversion is the `int` boundary, not the
store. Move the value across as `any` and add a regression that reads back
through the same boundary.

## Related

- [Pipeline](pipeline.md) - where lowering and runtime selection occur.
- [Values](values.md) - language-level value semantics.
- [Native](native.md) - ABI and pointer contracts.
