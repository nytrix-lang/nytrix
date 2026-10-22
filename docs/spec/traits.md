<!-- nytrix-doc: {"audience":"user","featured":true,"group":"spec","order":22,"summary":"Traits, impl blocks, attached methods, operator declarations, and law verification."} -->
# Traits and impl

Use `impl` to attach methods and operators to a type. A `trait` declares a
shared method surface; `impl Trait for Type` provides its implementation and
can include compile-time `law` checks.

Custom infix spelling lives in [Operators](operators.md). Stdlib receiver calls
such as `xs.append(...)` are documented module APIs; see
[Values](values.md#receiver-methods). They are not user-defined `impl` blocks.

## Where the compiler checks this

After Hindley-Milner typing, the **trait validation** stage runs before ABI
checks (`src/code/lang/types/pipeline/emit.c`, stage `NY_TYPE_PIPELINE_STAGE_TRAIT`).
That stage:

1. Validates every **`operator … = target`** declaration (target exists, two
   parameters, types line up, no duplicate `(op, left, right)` in scope).
2. Rejects repeated attached method names on the same owner
   (`impl-method-duplicate`). Attached-method overload selection by arity is
   not supported, so a second declaration with a different parameter count
   also fails instead of depending on signature-table order.
3. Requires a resolving operator impl for binary ops on **nominal** custom types
   when no built-in path applies (`operator-unresolved`, `operator-ambiguous`).

During **trait validation**, every **`law { … }`** is checked by the common
proof solver and compile-time Boolean evaluator before codegen prepare. This
validation is shared by NYIR, native, and LLVM routes. During **codegen
prepare**, each **`impl`** registers its owner type and attached method
signatures.

If trait validation fails, the driver reports **`trait validation failed`**
(`src/code/lang/pipeline/init.c`).

See [Pipeline](pipeline.md#trait-validation) and
[Implementation status](../learn/status.md).

## Attached methods (type-local)

```ny
use core

enum Shape {
   Circle(int radius),
   Rect(int width, int height),
   Empty
}

impl Shape {
   fn method_area(self s) int {
      match s {
         Shape.Circle(r) -> r * r
         Shape.Rect(w, h) -> w * h
         Shape.Empty -> 0
      }
   }
}

assert(Shape.Circle(5).method_area() == 25, "ADT method call")
```

- The parser stores methods as qualified names like `Shape.method_area`
  (`qualify_attached_method` in `src/code/lang/parse/stmt/core.c`).
- Call sites use **`value.method_name(...)`** (member call / receiver syntax).
- An **`impl`** body may contain **`fn`**, **`operator`**, and **`law`** entries.

Methods can also live on **`struct`** and **`layout`** declarations in addition
to freestanding **`impl`** blocks (same attachment rules).

## One impl, several owners

```ny
impl int, f64 {
   fn twice(self x) self { x + x }
}
```

The parser builds one **`impl`** for the first type, then clones the block for
each additional type in the comma list (`parse_impl_stmt`).

## Traits and trait implementations

Contract (signatures only):

```ny
trait Show {
   fn show(any v) str
}
```

Implementation:

```ny
use core

trait Show {
   fn show(any value) str
}

enum Point { Origin }

impl Show for Point {
   fn show(any p) str { return "Point" }
   law { 1 == 1 }
}

def pt = Point.Origin
assert(show(pt) == "Point", "trait dispatch")
```

- `trait` bodies allow signatures only, without executable bodies.
- `impl Trait for Type` supplies bodies. The `for` form sets `impl.trait_name`
  in the AST; plain `impl Type { … }` does not.
- Trait calls support free-function syntax (`show(value)`) as well as
  `value.show()`. Resolution ties the value type to a visible `impl Trait for Type`.

Fixtures: `etc/tests/lang/types/aggregates/types.nshape` (ADT methods),
`etc/tests/lang/types/refinements/refinements.nshape` (trait + law + `requires` samples).

## Law blocks

```ny
trait Show {
   fn show(any value) str
}

struct Point {
   int x,
   int y
}

impl Show for Point {
   fn show(any p) str { return "Point" }
   law { 1 == 1 }
}
```

Verification order:

1. Proof solver (`ny_proof_try_solver`, selected by `--proof-solver`; default
   `auto`, with bounded Presburger and optional Z3 support).
2. Fast compile-time boolean fold (`ny_try_eval_comptime_expr_fast`).
3. If neither step proves the law, trait validation rejects the impl,
   regardless of whether the owner is a struct or enum.

Failure otherwise: **`law verification failed in 'impl Trait for Type'`**.
Positive cross-backend fixture: `etc/tests/lang/types/traits/impl-law-portable.nshape`.
Negative cross-backend fixtures:
`etc/tests/lang/errors/type/impl-law-failed.nshape`.

## Operator declarations

Operators never change precedence. They only pick a **target function** for a
`(left type, right type)` pair.

### Inside `impl`

Left type is the impl owner. Omit the left operand type:

```ny
impl Meter {
   operator + self: self = add_meters
   operator ** int: self = pow_meters
   operator ^ self: self = xor_meters
}
```

`self` on the right side means the owner type (`parse_operator_stmt_with_left`).

### Top level

```ny
operator vec3 + vec3: vec3 = v_add
```

Form: **`operator` Left Op Right `:` Return `=` target**.

In expressions, **`^`** is XOR and **`**`** is exponentiation. Overload each
operator using its own fixed spelling.

### At call sites

The shared type/trait pipeline resolves each custom operator to its
declaration and reports unresolved or ambiguous nominal operations before
backend lowering. Attached-call lowering preserves the canonical method
identity selected by shared typing; the native NYIR path does not repeat a
receiver/name lookup. Built-in vector aliases are recognized in the language
type layer and shared by LLVM and native lowering. Same-owner attached-method
overloads by arity remain unsupported and fail closed.

## Static capability parameters (not traits)

Parameters such as **`numeric`**, **`sequence`**, **`indexable`**, **`iterable`**,
and **`allocator`** are **static capability constraints** on ordinary functions.
They are not dynamic trait objects:

```ny
fn len_of(sequence xs) int { len(xs) }
```

See `etc/tests/lang/types/aggregates/types.nshape` and [Types](types.md).

## Diagnostic cheat sheet

| Message | Likely fix |
| --- | --- |
| `trait validation failed` | Fix operator or impl-method diagnostics listed above it |
| `operator-target-missing` | Define the function named after `=` |
| `operator-target-arity` | Target must take exactly two arguments |
| `operator-unresolved` | Add `impl` operator or use types with a built-in op |
| `impl-method-duplicate` | Keep one declaration of each attached method name per owner; arity does not distinguish overloads |
| `law verification failed` | Fix a false law or express it within the supported proof calculus; unknown propositions are rejected |

## Related

- [Operators](operators.md)
- [Pipeline](pipeline.md)
- [Types](types.md)
- [Functions](functions.md)
- [Proofs](../learn/proofs.md)
- [Troubleshooting](../learn/troubleshooting.md)
