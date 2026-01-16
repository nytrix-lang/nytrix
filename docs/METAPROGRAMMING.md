# Metaprogramming in Nytrix

Nytrix currently supports compile-time execution via the `comptime` expression form. A `comptime { ... }` block is compiled with a temporary LLVM module, executed immediately, and its 64-bit return value is inlined as a constant in the surrounding program.

## Using `comptime`

```nytrix
X = comptime {
    a = 10
    b = 20
    return a + b
}

FLAGS = comptime {
    if 1 == 1 { return 3 }
    return 0
}

NESTED = comptime {
    INNER = comptime { return 5 }
    return INNER * 2
}
```

- Blocks can contain conditionals, loops, and nested `comptime` expressions.
- If a block falls through without `return`, the compiler inserts `return 0`.
