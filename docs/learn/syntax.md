<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":118,"summary":"Use syntax extensions when an ordinary function or template cannot express the rule."} -->
# Syntax

Macros, attributes, and rewriters extend source processing. Prefer ordinary
functions and compile-time generation when they express the same behavior.

## Choose the smallest tool

| Need | Use |
| --- | --- |
| Generate declarations from known data | Compile-time template or `emit`. |
| Attach a checked function contract | Attribute. |
| Introduce a source form | Macro. |
| Normalize an existing source form | Rewriter. |

## Workflow

1. Define one extension with a narrow source contract.
2. Format and inspect the affected source.
3. Run the smallest fixture that exercises expansion.
4. Add a diagnostic when an invalid form has a clear repair.

```bash
ny fmt --metaprog file.ny
ny fmt --check file.ny
ny --strict-types file.ny
```

The syntax registry owns macro, attribute, and rewriter expansion. Expansion
repeats to a fixed point only when each step remains valid. An unsupported
extension form reports a diagnostic rather than executing a fallback body.

## Register a macro

`std.core.syntax` owns process-wide and local registries. Register a named
macro handler, then expand it:

```ny
use std.core
use std.core.syntax as syntax

fn double_handler(any args) any { args }

mut reg = syntax.new_registry()
reg = syntax.register_macro_in(reg, "double", double_handler)
def expanded = syntax.expand_macro_in(reg, "double", [21])
assert(expanded == 21, "macro expanded")
```

Missing macro handlers return `nil`; missing attribute handlers return the
original node. Keep the handler contract narrow and test the expanded result
with a runtime assertion, not by trusting the expansion.

## Add a diagnostic rule

When an invalid source form has a clear repair, declare a compile-time
diagnostic rule next to the contract it protects:

```ny
comptime diagnostic rule bad_layout_store {
   when call.name == "store_layout" && !is_literal(call.arg(1))
   error "store_layout needs a string literal layout name"
   fix "use store_layout(dst, \"LayoutName\", ...)"
}
```

Use one clear error for one repairable condition. See
[Compile-time execution](../spec/comptime.md#diagnostic-rules) for the full
rule surface.

## Related

- [Comptime](comptime.md)
- [Compile-time execution](../spec/comptime.md)
- [Contributor guide](../CONTRIBUTE.md)