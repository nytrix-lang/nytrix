# Feature Ideas

This document outlines proposed features.

## Explicit Allocator Context
Implicitly pass a `Context` struct (containing an allocator pointer) to functions.

**Why:** Allows switching allocation strategies (e.g., to a simplified Arena) without changing function signatures or adding runtime overhead beyond a single pointer passing.

```nytrix
context.allocator = ArenaAllocator(1024)
x = [1, 2] ; Uses arena
```

## Functionality

**Auto-Parallelism**: Start thinking all more in a functional way.
