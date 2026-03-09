;; Keywords: alloc allocator memory
;; Allocation and lifetime operations for raw memory, arenas, and ownership-sensitive buffers.
module std.core.alloc(bump_new, bump_alloc, bump_alloc_aligned, bump_reset, bump_capacity, bump_used, bump_available, bump_mark, bump_release, new_allocator, allocator_name, allocator_state, set_allocator_state, heap_allocator, bump_allocator, new_context, init_context, context, set_context, context_allocator, set_context_allocator, with_context, ctx_alloc, ctx_realloc, ctx_free, ctx_zalloc)
use std.core

@inline
fn _ensure_bump_state(list: state): list {
   if(!is_list(state)){ panic("bump state must be a list") }
   if(state.len < 3){ panic("bump state must have [buffer, capacity, offset] layout") }
   state
}

fn bump_new(int: cap): list {
   "Create a new bump allocator with capacity `cap`. State layout is `[buffer, capacity, offset]`."
   if(cap < 0){ panic("bump capacity cannot be negative") }
   def buf = malloc(cap)
   if(!buf && cap > 0){ panic("bump allocation failed") }
   return [buf, cap, 0]
}

fn bump_alloc(list: state, int: n): ptr {
   "Allocates `n` bytes from bump allocator state. Returns 0 when capacity is exhausted."
   if(n <= 0){ return 0 }
   state = _ensure_bump_state(state)
   def base = state[0]  cap = state[1]  off = state[2]
   if(off + n > cap){ return 0 }
   def p = base + off
   state.set(2, off + n)
   p
}

fn bump_alloc_aligned(list: state, int: n, int: align=8): ptr {
   "Allocates `n` bytes aligned to `align` bytes from bump state. `align` must be a positive power-of-two."
   if(n <= 0){ return 0 }
   if(align <= 0){ panic("bump alignment must be positive") }
   if((align & (align - 1)) != 0){ panic("bump alignment must be a power-of-two") }
   state = _ensure_bump_state(state)
   def base = state[0]  cap = state[1]
   mut off = state[2]
   def rem = off % align
   if(rem != 0){ off += align - rem }
   if(off + n > cap){ return 0 }
   def p = base + off
   state.set(2, off + n)
   p
}

@inline
fn bump_capacity(list: state): int {
   "Returns total bump allocator capacity."
   state = _ensure_bump_state(state)
   state[1]
}

@inline
fn bump_used(list: state): int {
   "Returns currently used bytes in bump allocator."
   state = _ensure_bump_state(state)
   state[2]
}

@inline
fn bump_available(list: state): int {
   "Returns remaining free bytes in bump allocator."
   state = _ensure_bump_state(state)
   def cap = state[1]
   def off = state[2]
   if(off >= cap){ return 0 }
   cap - off
}

@inline
fn bump_mark(list: state): int {
   "Returns the current bump offset marker."
   bump_used(state)
}

fn bump_release(list: state, int: mark): bool {
   "Rewinds bump allocator to `mark`. Returns true when marker is valid, false otherwise."
   state = _ensure_bump_state(state)
   def off = state[2]
   if(mark < 0 || mark > off){ return false }
   state.set(2, mark)
   true
}

@inline
fn bump_reset(list: state): int {
   "Resets bump allocator offset to zero."
   state = _ensure_bump_state(state)
   state.set(2, 0)
   0
}

fn _heap_alloc(any: state, any: n): ptr {
   if(state){ state = state }
   if(!is_int(n) || n <= 0){ return 0 }
   malloc(n)
}

fn _heap_realloc(any: state, ptr: p, any: n): ptr {
   if(state){ state = state }
   if(!is_int(n) || n <= 0){ return 0 }
   realloc(p, n)
}

fn _heap_free(any: state, ptr: p): int {
   if(state){ state = state }
   free(p)
}

fn _bump_ctx_alloc(any: state, any: n): ptr {
   if(!is_int(n) || n <= 0){ return 0 }
   bump_alloc(_ensure_bump_state(state), n)
}

fn _bump_ctx_realloc(any: state, ptr: p, any: n): ptr {
   if(!p){ return _bump_ctx_alloc(state, n) }
   0
}

fn _bump_ctx_free(any: state, ptr: p): int {
   if(state || p){ return 0 }
   0
}

@inline
fn _ensure_allocator(list: allocator): list {
   if(!is_list(allocator)){ panic("allocator must be a list") }
   if(allocator.len < 5){ panic("allocator list must have at least 5 slots") }
   if(!allocator.get(0, 0)){ panic("allocator alloc callback is missing") }
   allocator
}

@inline
fn _ensure_context(list: ctx): list {
   if(!is_list(ctx)){ panic("allocation context must be a list") }
   if(ctx.len < 1){ panic("allocation context must expose allocator slot") }
   _ensure_allocator(ctx.get(0, 0))
   ctx
}

fn new_allocator(fnptr: alloc_fn, ?fnptr: realloc_fn=nil, ?fnptr: free_fn=nil, any: state=0, str: name="custom"): list {
   "Creates allocator descriptor `[alloc_fn, realloc_fn, free_fn, state, name]`."
   if(!alloc_fn){ panic("alloc_fn cannot be none") }
   def res = [alloc_fn, realloc_fn, free_fn, state, name]
   ; print("new_allocator: res=", res, " is_list=", is_list(res))
   return res
}

@inline
fn allocator_name(list: allocator): str {
   "Returns allocator name label."
   allocator = _ensure_allocator(allocator)
   allocator.get(4, "")
}

@inline
fn allocator_state(list: allocator): any {
   "Returns allocator state payload."
   allocator = _ensure_allocator(allocator)
   allocator.get(3, 0)
}

fn set_allocator_state(list: allocator, any: state): list {
   "Updates allocator state payload."
   allocator = _ensure_allocator(allocator)
   allocator.set(3, state)
   allocator
}

fn heap_allocator(): list {
   "Creates a heap-backed allocator descriptor using std.core malloc/realloc/free."
   new_allocator(_heap_alloc, _heap_realloc, _heap_free, 0, "heap")
}

fn bump_allocator(int: cap): list {
   "Creates an arena-style bump allocator descriptor."
   new_allocator(_bump_ctx_alloc, _bump_ctx_realloc, _bump_ctx_free, bump_new(cap), "bump")
}

fn new_context(any: allocator=0): list {
   "Creates allocation context list `[allocator]`. Defaults to heap allocator."
   if(!allocator){ allocator = heap_allocator() }
   _ensure_allocator(allocator)
   return [allocator]
}

mut __context = 0

fn init_context(): list {
   "Initializes and returns the process-wide default allocation context."
   if(!__context){ __context = new_context() }
   __context
}

@inline
fn context(): list {
   "Returns process-wide default allocation context."
   init_context()
}

fn set_context(list: ctx): list {
   "Replaces process-wide default allocation context."
   __context = _ensure_context(ctx)
   __context
}

fn _resolve_context(any: ctx=0): list {
   if(!ctx){ return init_context() }
   _ensure_context(ctx)
}

@inline
fn context_allocator(any: ctx=0): list {
   "Returns allocator descriptor bound to context `ctx` (or global default when omitted)."
   ctx = _resolve_context(ctx)
   _ensure_allocator(ctx.get(0, 0))
}

fn set_context_allocator(list: allocator, any: ctx=0): list {
   "Binds `allocator` to context `ctx` (or global default context when omitted)."
   allocator = _ensure_allocator(allocator)
   ctx = _resolve_context(ctx)
   ctx.set(0, allocator)
   ctx
}

fn with_context(list: ctx, fnptr: thunk): any {
   "Runs `thunk()` with temporary process context `ctx`, then restores previous context."
   ctx = _ensure_context(ctx)
   if(!thunk){ panic("with_context requires a callable thunk") }
   def prev = __context
   __context = ctx
   defer { __context = prev }
   thunk()
}

fn ctx_alloc(int: n, any: ctx=0): ptr {
   "Allocates `n` bytes through allocator context `ctx` (or process context by default)."
   if(n <= 0){ return 0 }
   def allocator = context_allocator(ctx)
   def alloc_fn = allocator.get(0, 0)
   alloc_fn(allocator_state(allocator), n)
}

fn ctx_realloc(ptr: p, int: n, any: ctx=0): ptr {
   "Reallocates `ptr` to size `n` through allocator context `ctx`."
   if(n <= 0){ return 0 }
   def allocator = context_allocator(ctx)
   def realloc_fn = allocator.get(1, 0)
   if(!realloc_fn){
      if(!p){ return ctx_alloc(n, ctx) }
      return 0
   }
   realloc_fn(allocator_state(allocator), p, n)
}

fn ctx_free(ptr: p, any: ctx=0): int {
   "Frees `ptr` through allocator context `ctx`. Missing free callback is treated as no-op."
   if(!p){ return 0 }
   def allocator = context_allocator(ctx)
   def free_fn = allocator.get(2, 0)
   if(!free_fn){ return 0 }
   free_fn(allocator_state(allocator), p)
}

fn ctx_zalloc(int: n, any: ctx=0): ptr {
   "Allocates `n` bytes and zero-fills memory through allocator context `ctx`."
   def p = ctx_alloc(n, ctx)
   if(!p){ return 0 }
   memset(p, 0, n)
   p
}
