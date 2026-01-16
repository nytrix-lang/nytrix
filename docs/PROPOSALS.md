# Feature Ideas

This document outlines proposed features to enhance Nytrix's ergonomics and systems capabilities without compromising its minimalist philosophy or binary size.

### `embed` (Compile-time Resources)
Include binary files directly into the executable data section. Avoids runtime file I/O for static assets.

**Syntax:**
```nytrix
def SHADER = embed("gfx/shader.glsl") ; returns Bytes pointer
```

## Explicit Allocator Context
Implicitly pass a `Context` struct (containing an allocator pointer) to functions.

**Why:** Allows switching allocation strategies (e.g., to a simplified Arena) without changing function signatures or adding runtime overhead beyond a single pointer passing.

```nytrix
context.allocator = ArenaAllocator(1024)
x = [1, 2] ; Uses arena
```

## Functionality

**Auto-Parallelism**: Start thinking all more in a functional way.
