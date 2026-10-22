<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":118,"summary":"Choose between ordinary functions, compile-time generation, and source extensions."} -->
# Syntax extensions

Use the narrowest mechanism that expresses the requirement. A macro or rewriter
changes source processing and is harder to reason about than an ordinary
function or generated declaration.

| Need | Prefer |
| --- | --- |
| Compute a compile-time value | `#run`, `#comptime`, or `static_assert` |
| Generate declarations from known inputs | `comptime template` and `comptime emit` |
| Check a declaration contract | A supported function attribute |
| Introduce a new source form | A macro handler |
| Rewrite an existing form | A deterministic rewriter |

## Workflow

1. Write down the source forms the extension accepts and the output it emits.
2. Keep the transformation deterministic and bounded.
3. Give invalid forms a source-located diagnostic and a useful repair.
4. Inspect the expansion and test both a valid input and a rejected input.
5. Compile and run generated code on the intended backend.

The syntax registry API lives under `core.compiler.syntax`. Search the current
checkout for its documented operations rather than copying an incomplete
handler example:

```bash
ny doc get core.compiler.syntax
ny doc search --symbols register_macro_in
ny fmt --metaprog file.ny
```

`ny fmt --metaprog` is an inspection aid, not proof that an extension's output
is correct. See [Compile-time programming](comptime.md) for generated
declarations and [Comptime](../spec/comptime.md#syntax-extension-registry)
for the registry boundary.
