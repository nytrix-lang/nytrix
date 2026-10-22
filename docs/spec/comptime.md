<!-- nytrix-doc: {"audience":"user","featured":false,"group":"spec","order":190,"summary":"Compile-time evaluation, generated values, and the boundaries of comptime execution."} -->
# Comptime

Compile-time forms run during compilation and produce tables, matches, templates, emitted declarations, and generated modules.

## Compile-time blocks

```ny
comptime { body }
```

Rules:

- Runs during compilation
- Returns a value embedded in the program
- Uses the same resolved imports and aliases as the surrounding source unit
- Emits only the standard-library helpers reached by the block
- Can reuse earlier immutable compile-time constants
- Does not run unrelated top-level user code
- Does not capture runtime globals

```ny
use core

def xs = comptime{ range(4).map(fn(i){ i + 1 }) }
assert_eq(to_str(xs), "[1, 2, 3, 4]", "comptime imports")
```

`#run expr` and `#comptime expr` evaluate an expression during compilation.
Both also accept a braced body when multiple statements are needed. The
braced and unbraced forms lower to the existing compile-time expression; they
do not introduce a separate evaluator or change capture rules. Braces are
useful for grouping statements, but are not required around a single
compile-time expression.

```ny
use core

def base = #run 2 ** 5
def values = #run range(4).map(fn(i){ i + base })
def also_base = #comptime 2 ** 5
def doubled = #comptime { return base * 2 }
```

```ny
use core

def base = comptime{ 2 ** 5 }
def xs = comptime{ range(4).map(fn(i){ i + base }) }
```

For runtime data, put the value inside the block or bind it first as an immutable compile-time value.

`return expr` or a final value-producing expression sets the value of a compile-time block. **A block with only declarations or non-value statements evaluates to `nil`.**

## Tables

```ny
comptime table Name {
   pattern -> value
   _ -> fallback
}
```

`#table Name { ... }` is the concise spelling of a comptime table.

Compile-time tables describe static dispatch and generated classifiers. Their
declaration does not guarantee branch-free output.

The compiler also emits a compatibility helper named from the table. For `comptime table KeyMap`, call `_key_map(key)` or `_key_map(key, fallback)`. Prefer `comptime match KeyMap(key, fallback)` in new code.

## Match helpers

```ny
comptime match Name(key, fallback)
```

`comptime match` queries a compile-time table by key and returns a fallback when no arm matches.
`#match Name(key, fallback)` is its concise spelling.

## Templates

```ny
comptime template name(args) {
   declarations
}
```

`#template name(args) { ... }` is an alias for this declaration.

Hygienic templates generate declarations as AST nodes. Use `comptime template`
for a generator, `comptime emit` for insertion, `quote { ... }` for syntax,
and `${value}` for a splice. The same splice form is used for template
parameters, quoted AST insertion, and scalar interpolation inside template
string literals; its meaning follows the destination slot. The former
`quote $ { ... }` and `quote ${ ... }` quote openers are rejected. `quote` is
contextual: it is the quote form only before a braced body, so a normal
identifier or call named `quote` remains available elsewhere. The `#template`,
`#insert`, and `#emit` spellings remain accepted as concise aliases.

`gensym(prefix)` produces a deterministic string name. Outside named modules
it uses `__ny_gensym_N_prefix`. Inside a named module it uses
`__ny_gensym_mHEX_N_prefix`, where `HEX` reversibly encodes the complete module
name as lowercase hexadecimal UTF-8 bytes. Each module has its own increasing
`N`, so parsing another module does not change its sequence. Interpolation and
isolated declaration parsing share the enclosing sequence. Prefixes are never
truncated; exhausted counters fail before wrapping. These names remain
strings, rather than scope-marked `code<ident>` values.

`quote` produces syntax that can be spliced into a template. It preserves scope
marks and source origins and checks expression, statement, and declaration
categories at insertion. The library provides typed
`core.compiler.ast.Code<T>` values with distinct `CodeExpr`, `CodeStmt`, `CodeDecl`,
and `CodeIdent` marker types. `code_expr`, `code_stmt`, `code_decl`, and
`code_ident` validate the syntax category before constructing a value; typed
accessors and category-specific JSON decoders validate the versioned
`nytrix.code.value` envelope before returning or restoring data. These values
can be stored and passed through ordinary Ny functions. Direct constructor
calls over quoted syntax can be template arguments and are checked against the
template parameter category. Bounded pure helper calls may return a typed
constructor result such as `Result<Code<CodeExpr>, str>`, or its unwrapped
`Code<CodeExpr>`, when the helper's body only forwards its syntax argument
through immutable locals and a final return. This is a template-boundary
evaluation of quoted syntax; runtime generated `Code<T>` values cannot be
spliced directly with `${...}`.
Compile-time shortcuts for `parse_code`, `codeof`, code queries, typed code
constructors/accessors, `ident`, and typed-code `unwrap` yield to same-named
declarations in the current file or module, preserving ordinary local name
shadowing.

Template parameters can constrain compile-time substitutions with
`value: code<expr>`, `code<stmt>`, `code<decl>`, or `code<ident>`; mismatched
scalar and homogeneous sequence categories are rejected before cloning. For
example, `code<expr>` accepts a quoted expression or a homogeneous list of
quoted expressions, including an empty list, while rejecting a quoted
declaration. Empty sequences can fill expression, statement, and declaration
sequence slots, but cannot satisfy scalar `code<ident>`. `core.compiler.ast.code_str`
returns compact canonical JSON with recursively sorted object keys, stable
floating-point round-trips, and preserved snapshot origins and syntax marks.
`code_category` reports the category from the versioned syntax record.
Container nesting is bounded to 64 levels. Runtime-created code cannot flow
backward into template expansion. Some AST origin and compiler-fact reflection
coverage also remains limited.

Template parameters and quote splices share the `${...}` spelling. In a
template body, a quoted code argument inserts one expression in a scalar
expression position. In an expression list, positional call arguments, or a
statement sequence, a list argument expands into that sequence; each element
must match the destination category. A braced quote containing one expression
can fill an expression slot. Inside `quote`, `${...}` is parsed as an AST splice
even when the quote appears in a template. Expanded call argument counts are
checked by the normal call validator after expansion; too few and too many
arguments both fail compilation.

### Generated identifiers

For generated names, a bare symbol argument is already an identifier. Convert
a compile-time string or scalar explicitly with `ident(value)`:

```ny
comptime template make_axis(suffix, value) {
   fn axis_${suffix}() str { return value }
}

comptime emit make_axis(ident("x"), "x")
```

Inside a reflected `comptime fields(Layout)` emission, identifier-valued
facts may also be spliced into declaration names: `${field.name}` and
`${field.type}` (equivalently `${field.type_name}`). These values come from
compiler-owned layout metadata; arbitrary strings and scalar values still
require `ident(...)` and retain the same validation and scope-mark rules.

Layout reflection exposes `field.default_expr` as the declared initializer
syntax, or `nil` when absent. `field.default_value` yields that initializer or
the compiler's implicit primitive default. This permits Ny generators to
preserve declared defaults without parsing source strings.

`ident` accepts a nonempty fragment of ASCII letters, digits, and underscores.
Each segment of the completed name must start with a letter or underscore and
contain only those characters after its first character. A string or scalar
passed without `ident(...)` is rejected in a generated name or type position.
A bare symbol passed to `ident(...)` retains that symbol's scope mark; a string
fragment receives the scope mark of the `ident(...)` call. When spliced as
expression code, that mark is resolved exactly, so a same-spelled binding
introduced by the template cannot capture it. At a `code<ident>` template
parameter, a quoted expression is rejected and a bare symbol or `ident(...)`
argument retains exact caller lookup. The `code_ident` library constructor
likewise accepts only identifier syntax, not an unmarked string.

In a template string literal, `${value}` inserts the text of a string,
identifier, integer, or boolean template argument. This is value interpolation
inside the string and does not parse or capture code. It also lets compiler
generated helper strings refer to a layout name that is supplied when the
surrounding template expands.

### Parsing and inspecting syntax

`core.compiler.ast.parse_code(source)` is the declaration-only companion to
`parse_ast`. It returns versioned declaration records only when the source is
well formed and every top-level node is a declaration; empty input, executable
statements, and syntax errors return nil. `parse_code_typed` returns a
`Result<Code<CodeDecl>, str>` for the same isolated parse and never exposes
partial declarations.

`code_kind(node)`, `code_name(node)`, and `code_has_attribute(node, name)` query
these AST records at runtime. As template arguments, the same queries
inspect `quote`, `parse_code`, and `codeof` syntax directly without converting
it to JSON. `code_has_attribute` checks one declaration's preserved attributes.
`code_kind` reports
`"declarations"` for a syntax sequence; `code_name` returns an empty string
for non-declarations, unnamed declarations, or a sequence containing multiple
declarations. `code_kind` returns `"invalid"` for non-code scalar values, and
`code_has_attribute` returns `false` unless its input is exactly one declaration.
The template-time queries use the same neutral results for unclassifiable
values; malformed calls and nonliteral attribute names still produce compile
errors.

As a `comptime template` argument, the unqualified `parse_code("...")` form
parses a literal source string in an isolated parser and produces a sequence of
declaration AST nodes for `${...}` insertion. It rejects nonliteral input,
embedded NUL bytes, malformed source, empty source, executable top-level
statements, and assignment statements before inserting anything. Parsed
declarations inherit the active module and syntax context. Tokens keep the
source filename of the string
literal; line and column coordinates start within the supplied source text.
Declarations introduced by this isolated parse do not leak into the caller's
`codeof` registry. Inserted nodes continue through ordinary typing and code
generation. This staged argument form supplies AST for insertion; the
library's typed `Code<T>` values are constructed separately through
category-checked APIs.

In a template argument, `codeof(name)` captures the AST of a previously
declared named file- or module-level declaration. Its result can be inserted
into a declaration sequence with `${...}`; expression positions reject
declaration code, and unqualified names that resolve ambiguously require
module qualification. The captured syntax retains its source token and scope
marks. `codeof` supplies syntax for insertion, not compiler-resolved semantic
facts.

## Library code generation

Use `core.compiler.codegen` for generators that should be ordinary library APIs.
`json_for(Layout)` emits a helper that loads each field into a dictionary
keyed by field name. Pass that value to a JSON encoder when serialized text is
needed. `soa_for(Layout)` emits a helper that transposes a list of pointers to
the layout into a dictionary of per-field lists.
`table_gen(Layout)` emits a pure adapter returning a dictionary with `headers`
and `rows`; both follow field declaration order, and each row contains
stringified field values. Pass the result to `core.term.table` or another
renderer.

`defaults_for(Layout)` emits `Layout_defaults()`, a dictionary containing each
field's effective default. `store_dict_for(Layout)` emits
`Layout_store_dict(ptr, values)`, which stores a field-name dictionary into an
existing allocation, supplies defaults for omitted fields, and rejects an
unknown key before writing any field. `new_for(Layout)` emits
`Layout_new(values)`, allocating an instance with effective defaults and
applying supplied named fields through that writer; invalid keys return null
and release the allocation.

`store_fields_for(Layout)` emits
`Layout_store_field(ptr, field_name, value)`, which writes one named field
through the compiler-checked layout ABI and returns false for an unknown field
or null destination. It also emits a typed `Layout_store_<field>(ptr, value)`
writer for each field, so callers can avoid string-based field selection and
receive normal argument type checking. Storage type, offset, and conversion
come from the declared layout, not caller metadata.
Generated `Layout_zero`, `Layout_load_fields`, and `Layout_json_value` return
null or an empty dictionary for allocation failure/null input rather than
dereferencing a null pointer.

```ny
use core.compiler.codegen (json_for, soa_for, table_gen)

layout Packet {
   i32 id
   bool active
}

comptime emit json_for(Packet)
comptime emit soa_for(Packet)
comptime emit table_gen(Packet)
```

The generated declaration goes through normal typing and code generation. Function
`requires`, `decreases`, and `@proves` formulas are cloned and spliced for
each emission, then processed by the ordinary contract validators. Template
expansion does not supply a proof certificate or bypass termination checking.
Refinement predicates, including existential bodies, trait methods, and
implementation laws also undergo template substitution and normal validation.
External signatures and lemma propositions are cloned per emission as well.
Decorators retain independently cloned argument nodes, so separate expansions
do not share a mutable annotation tree. Generated lemmas are still checked
as universal claims by the ordinary lemma validator.
Nested `try` bodies, handlers, deferred cleanup, and while-loop invariants
are cloned recursively. Versioned loop snapshots expose the parsed `invariant`
expression and its origins; this syntax record is not a proof certificate.
`match_gen` handles unit and payload enums through typed match patterns. The
syntax snapshot and semantic artifact APIs below expose different information:
parsed source structure and checked compiler facts, respectively.

### Syntax snapshots

`core.compiler.ast.parse_ast_snapshot(source)` returns a versioned syntax envelope
with schema `nytrix.ast`, version `1`, and a `nodes` list. Each syntax node has
a `source_origin` record containing its source filename, line, and column.
Template-generated nodes also carry an `expansion_origin` record for the
emission site while retaining their definition site in `source_origin`.
Selected `use` import names and aliases, and individual module export names,
also retain their own source and template-emission origins in these snapshots.
Generated declarations belong to the calling module and undergo normal type
validation there. Versioned statement records contain an ordered `attributes`
list; each attribute records its name, parsed argument nodes, and source origin.
Generated attributes also carry their emission origin. Function records add
nullable `requires`, `decreases`, and `doc` fields. These are parsed syntax,
not independently checked proof or effect evidence. Refinement records include
base types, bound parameters, and predicates, including existential binders.
Trait records include method signatures. Versioned enum records add generic
parameters with declaration origins, indexed-family information, and per-variant
typed payload fields with source and generated emission origins;
implementation records add the implemented trait and law expressions. Versioned
layout and struct field records retain array-extent and default-expression
syntax plus field-name source origins; generated fields also carry emission
origins. Layout records add deftype parameters, defaults, flavor, and methods;
struct records also retain methods. These fields describe source declarations,
not target-dependent computed sizes or offsets. Versioned statements
retain their `syntax_ctx` binding mark. Variable records also expose declaration,
mutability, destructuring, and type-annotation metadata. Identifier and quote
marks retain the difference between quoted bindings and outer-scope splices.
Versioned `use` records expose locality, wildcard-import state, and the ordered
selected-import names and aliases; generated imports retain source and emission
origins. The legacy `parse_ast` record shape is unchanged.
Versioned parameter records also retain their binding mark and declaration
origin. Template-cloned function, lambda, and layout parameters carry the
emission origin alongside their original definition site.
For integer literals outside signed 64-bit range, both AST APIs return
`integer_outside_i64: true`, a null `value`, and exact normalized `numeric_text`
with its radix prefix; the saturated internal parser payload is never exposed
as the literal value. String literal and formatted-string part serialization
is byte-counted: control bytes are escaped, and embedded NUL retains bytes
after it through JSON decoding and canonical serialization. Template
string interpolation and string-valued AST splices also preserve counted
bytes; `ident(...)` rejects embedded NUL because it cannot form an identifier.
The legacy `parse_ast` list shape remains unchanged. Syntax snapshot records
are decoded JSON values; compiler semantic facts are exposed by the separate
typed-artifact API below.

### Semantic artifacts

The compiler's HM artifact contains the type, representation, ownership,
effect, layout, origin, and proof-obligation records used by tooling. The
`core.compiler.reflect.semantic` module reads a `typed_ast.v1` HM artifact emitted
with `--stop-after=hm --emit-artifact=PATH` and exposes its typed facts as a
`nytrix.reflect.semantic` version-1 envelope. `semantic_get(record, key)` reads
a field and `semantic_items(sequence)` reads sequence items. Every JSON object
and array inside the returned snapshot is converted recursively to a tuple, so
the semantic records cannot be mutated. Unknown artifact versions and
malformed JSON return a structured error. `semantic_diagnostics(snapshot)`
returns immutable HM diagnostics, while `semantic_find_diagnostic` selects by
stable code and optional file/line/column. An ambiguous query returns nil
instead of choosing a record arbitrarily.

## Emit

```ny
comptime emit name(args)
for axis in comptime ["x", "y", "z"] {
   emit make_axis_family(axis)
}
```

`emit` inserts generated declarations from a template or generator. `#insert`
and `#emit` are aliases for template insertion. Insertion is allowed at any
statement position, including module scope; generated nodes are checked in
that position by the ordinary compiler pipeline. There is no separate
emit-context mode. Compile-time `for` iterates a compile-time list and lets
the body emit declarations for each value.

```ny
for axis in comptime ["x", "y"] {
   #insert make_axis_family(axis)
}
```

## Reflection loops

Template functions may also splice the ordered field list into their parameter
list. By default, each parameter carries the field's declared default or its
implicit scalar default. Add `required` when every generated parameter must be
provided, as for a fixed-arity store function:

```ny
comptime template store_for(Layout) {
   fn ${Layout}_store(ptr out, comptime fields(Layout, required)) ptr {
      ; generated body
   }
}
```

Field-sequence parameters preserve field types, declaration order, source
origins, and the splice's syntax context. The generated function then passes
through ordinary type checking and backend lowering.

```ny
comptime fields(LayoutName) as f {
   emit assert(__layout_offset("LayoutName", f.name) == f.offset, "field")
}

comptime exports(ModuleName) as name {
   emit assert(name != "", "export name")
}

comptime variants(Mode) as variant {
   emit assert(variant.name != "", "variant name")
}
```

`fields` exposes stable metadata for each layout field:

| Property | Value |
| --- | --- |
| `f.name` | Field name string. |
| `f.type` / `f.type_name` | Declared type-name string. |
| `f.offset` | ABI byte offset. |
| `f.index` | Zero-based declaration index. |
| `f.is_array` | Whether the field uses `[T, N]` syntax. |
| `f.array_len_known` | Whether `N` is an integer literal available to reflection. |
| `f.array_len` | Literal array extent, or `0` when it is not statically represented. |
| `f.has_default` | Whether the field declares a default. |
| `f.default_src` | Stable source spelling of that default, or `""`. |
| `f.align` | Explicit field alignment, or `0` for natural alignment. |

Use `array_len_known` before consuming `array_len`, since a symbolic extent
and the literal extent zero must not be conflated. `exports` exposes each
exported module name as a string.
`variants` exposes `variant.name`, declaration-order `variant.index`,
`variant.has_payload`, and `variant.payload_count`. In a match pattern slot,
`variant.pattern` expands to the checked enum constructor pattern with one
wildcard per payload field. Using this property outside a match pattern is
an error. The `#fields(...)`,
`#exports(...)`, and `#variants(...)` spellings are aliases for these
reflection loops.

`comptime attributes(Declaration) as attribute` iterates declaration
attributes in source order, binding each item to its name. The initial
`core.reflect.attributes_for(Declaration)` API packages the ordered names
in an immutable `nytrix.reflect.attributes` version-1 tuple.

`core.compiler.ast.code_signature(code)` inspects function, extern, and anonymous
`fn` syntax as an immutable versioned tuple. Parameter defaults are canonical
AST JSON strings, with parameter origins retained. `code_contract(code)`
returns canonical JSON strings for a function's `requires` and `decreases`
clauses.

`core.compiler.codegen.match_gen(Enum)` emits `Enum_variant_name(Enum value)` for
unit and payload enums. It inspects the variant without reading its payload;
arguments from a different enum are rejected by normal type checking.

Inside a `#fields(Layout) as field` insertion, `value.(field)` is a
compile-time member splice. It expands to a `load_layout(value, "Layout",
"field-name")` access using the active descriptor; the containing function's
declared result type supplies the normal typed boundary. A member splice
outside that descriptor scope is rejected with a source-located diagnostic.

## Diagnostic rules

```ny
comptime diagnostic rule bad_layout_store {
   when call.name == "store_layout" && !is_literal(call.arg(1))
   error "store_layout needs a string literal layout name"
   fix "use store_layout(dst, \"LayoutName\", ...)"
}
```

Diagnostic rules let compile-time code reject a known bad call pattern. The current rule surface supports call predicates such as `call.name`, `call.arg(N)`, and helpers such as `is_literal`.
`#diag rule name { ... }` is an alias for a comptime diagnostic rule.

## Compile-time proofs

Compile-time assertions move safety checks into compilation. The `proof` type in [Types](types.md) provides the carrier for dependent and refinement facts backed by the same engine.

```ny
assert_compile((4 * 11) == 44, "folded arithmetic")
static_assert((3 * 7) == 21, "folded arithmetic")
assert_compile_range(i, 0, 3, "loop index range")
assert_compile_index(xs, i, "list index bounds")
def proof arithmetic = prove((4 * 11) == 44, "arithmetic witness")
```

`static_assert` and `assert_compile` fail compilation when the condition is known false. `assert_compile_range` requires the compiler to prove an integer is within a closed range. `assert_compile_index` requires the compiler to prove that an index is in bounds for the container. `range_proven(value, lo, hi)` and `index_proven(container, index)` expose the same proof engine as compile-time booleans.

`prove(condition[, message])` is the only implicit-free constructor for a proof. It rejects false obligations and obligations whose truth is not known during compilation. `prove` produces a proposition-indexed proof type; for example, `prove(5 > 0)` has type `proof<5 > 0>`. Proofs have no inspectable runtime payload and are erased after checking.

`fn lemma name(params) { proposition }` names a reusable proposition. Its body must contain one proposition. The implication spelling `A → B` is normalized as `!A || B`; an application such as `positive_sum(3, 4)` can be passed to `prove` to construct `proof<positive_sum(3, 4)>`.

The current kernel normalizes equality symmetry (`a == b` and `b == a`) and reversed ordered comparisons (`n > 0` and `0 < n`). It does not turn a runtime boolean into a proof and does not attempt general theorem proving.

Use these checks at safety boundaries: parser tables, byte decoders, crypto code, native buffers, and loops where an out-of-range value would break memory or correctness.

`--safe-mode` uses the same proof engine for compiler-tracked raw memory accesses. If an allocation size is known, `load8`/`store8` and wider raw loads/stores require a byte offset proven to stay inside the allocation.

See [Types](types.md) for `proof` type, dependent params, and refinement.

## Generated modules

```ny
module pkg.generated generated from Spec {
   native_prefix = "x"
   emit make_backend(Contract)
}
```

Generated modules attach compile-time configuration to a module and emit the declarations that become part of the module surface.
The concise `#insert` spelling can insert a quoted declaration in their body.

## Boundaries

### Evaluation bounds

Compile-time evaluation has these bounds:

- Recursion is limited to 64 levels; deeper evaluation returns `false`.
- Ranges contain at most 65,536 elements.
- Fast-evaluator arithmetic uses checked (`__builtin_*_overflow`) or bounded operations.

The evaluator also has a deterministic compilation-wide fuel budget. It
defaults to 1,000,000 evaluation steps and can be set with
`--comptime-limit=N` (or `NYTRIX_COMPTIME_FUEL=N`). Exhaustion produces a
diagnostic instead of allowing type checking or generation to diverge. The
recursion and range limits above remain independent hard bounds.

### When compile-time code re-runs

Compile-time blocks run when their source unit is compiled. The compiler's
module and whole-program artifact caches may reuse compiled outputs when their
keys match the source, resolved imports, compiler identity, and relevant
options. Within one codegen context, the LLVM fast evaluator also memoizes up
to 128 successful expression results. It admits deterministic literals,
operators, containers, indexing/slicing, selected intrinsic calls, closed
container `.get` calls, immutable globals, and shared-type-checked pure
function calls. Keys use structural hashes followed by structural equality;
they include resolved immutable binding and pure function declaration identity
and all argument expressions. Target queries, `__main`, and registered
refinements remain uncached because they depend on compiler or typing context.
Cached heap values are
cloned, and a cache hit consumes the shared comptime fuel budget.

Each module worker owns its own expression memo, so parallel module compilation
does not share mutable cache state. Seeded `#run` output is checked against
serial and one-/four-worker compilation by `module-cache-equivalence`.
Unresolved calls, calls that are not proved pure, stateful random calls, and
unsupported expression forms remain uncached and use the regular evaluator.
Generated names must derive from declaration context so diagnostics and
imports stay stable across recompiles.

### Isolated compiler work

The compiler does not expose parse, type-check, or compile entry points to
`#run`. The Ny compound-assignment desugar driver runs in a separate compiler
process, so it owns its parser, diagnostics, options, module state, caches, and
LLVM state. A thread-local guard rejects recursive entry in the same session.

That invocation has hard limits: one MiB of input and generated source, 65,536
AST nodes, 64 nested AST containers, and the shared one-million-step comptime
fuel limit. The child compiler runs single-threaded and is bounded to 10 CPU
seconds, 15 wall-clock seconds, 1 GiB of address space, and one MiB of combined
output. Failed desugaring returns no rewritten source; the parent compiles only
the complete rewritten text after the child exits successfully.

Compiler-driver APIs callable from `#run` are not available.

## Platform selection

Platform guards select source at compile time:

```ny
#linux { os_tag = "linux" }
#elif macos { os_tag = "macos" }
#elif windows { os_tag = "windows" }
#else { os_tag = "other" }
#endif

#if(arch() == "x86_64"){ return 64 }
```

Guard names include OS families and CPU families. `comptime{ ... }` can call compile-time facts such as `arch()`, `os()`, `__os_name()`, and `__main()`.

## Embedded files

```ny
def text = embed("etc/tests/lang/language/embed.nshape")
```

`embed(path)` reads a file into the compiled program. Paths are resolved from the source checkout or active source root used by the compiler.

## Syntax extension registry

`core.compiler.syntax` exposes the runtime/comptime syntax registry used by the extension tests:

```ny
use core.compiler.syntax as syntax

mut reg = syntax.new_registry()
reg = syntax.register_macro_in(reg, "double", handler)
syntax.expand_macro_in(reg, "double", [21])
```

The surface covers process-wide and local registries, macro handlers, attribute handlers, form construction, deep/fixpoint expansion, registry clone/merge operations, and deterministic rewrite passes. Missing macro handlers return `nil`; missing attribute handlers return the original node.

## Result ownership

Heap results cross the comptime boundary by value. Strings, lists, tuples, ranges, and dictionaries are reconstructed in the receiving program before the temporary evaluator is destroyed; pointers into evaluator-owned storage are never exposed. Nested containers are supported with bounded depth and size. Invalid intermediate or generated evaluator modules are rejected before JIT execution.

## Debugging generated code

`ny --expand file.ny` shows expanded source. Compile and run that source on the
intended backend when isolating a generation error; expansion alone does not
check runtime behavior.

## Related

- [Units](units.md#generated-source) for generated module syntax.
- [Comptime](../learn/comptime.md) for practical usage.
- [Tooling](../learn/tooling.md) for `ny fmt --metaprog`.
