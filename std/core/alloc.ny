;; Keywords: core alloc
;; Core Alloc module.

module std.core.alloc (
   bump_new, bump_alloc, bump_alloc_aligned, bump_reset,
   bump_capacity, bump_used, bump_available,
   bump_mark, bump_release,
   new_allocator, allocator_name,
   allocator_state, set_allocator_state,
   heap_allocator, bump_allocator,
   new_context, context, set_context,
   context_allocator, set_context_allocator,
   with_context,
   ctx_alloc, ctx_realloc, ctx_free, ctx_zalloc
)
use std.core *

fn _ensure_bump_state(state){
   "Internal helper. Validates bump allocator state layout."
   if(!is_list(state)){ panic("bump state must be a list") }
   if(len(state) < 3){ panic("bump state must have [buffer, capacity, offset] layout") }
   state
}

fn bump_new(cap){
   "Create a new bump allocator with capacity `cap`. State layout is `[buffer, capacity, offset]`."
   if(cap < 0){ panic("bump capacity cannot be negative") }
   def buf = malloc(cap)
   return [buf, cap, 0]
}

fn bump_alloc(state, n){
   "Allocates `n` bytes from bump allocator state. Returns 0 when capacity is exhausted."
   if(n <= 0){ return 0 }
   state = _ensure_bump_state(state)
   def base = state[0]  cap = state[1]  off = state[2]
   if(off + n > cap){ return 0 }
   def p = base + off
   set_idx(state, 2, off + n)
   p
}

fn bump_alloc_aligned(state, n, align=8){
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
   set_idx(state, 2, off + n)
   p
}

fn bump_capacity(state){
   "Returns total bump allocator capacity."
   state = _ensure_bump_state(state)
   state[1]
}

fn bump_used(state){
   "Returns currently used bytes in bump allocator."
   state = _ensure_bump_state(state)
   state[2]
}

fn bump_available(state){
   "Returns remaining free bytes in bump allocator."
   state = _ensure_bump_state(state)
   def cap = state[1]
   def off = state[2]
   if(off >= cap){ return 0 }
   cap - off
}

fn bump_mark(state){
   "Returns the current bump offset marker."
   bump_used(state)
}

fn bump_release(state, mark){
   "Rewinds bump allocator to `mark`. Returns true when marker is valid, false otherwise."
   state = _ensure_bump_state(state)
   def off = state[2]
   if(mark < 0 || mark > off){ return false }
   set_idx(state, 2, mark)
   true
}

fn bump_reset(state){
   "Resets bump allocator offset to zero."
   state = _ensure_bump_state(state)
   set_idx(state, 2, 0)
   0
}

fn _heap_alloc(state, n){
   "Internal helper. Heap-backed alloc callback."
   if(state){ state = state }
   if(n <= 0){ return 0 }
   malloc(n)
}

fn _heap_realloc(state, ptr, n){
   "Internal helper. Heap-backed realloc callback."
   if(state){ state = state }
   if(n <= 0){ return 0 }
   realloc(ptr, n)
}

fn _heap_free(state, ptr){
   "Internal helper. Heap-backed free callback."
   if(state){ state = state }
   free(ptr)
}

fn _bump_ctx_alloc(state, n){
   "Internal helper. Bump-backed alloc callback."
   if(n <= 0){ return 0 }
   bump_alloc(state, n)
}

fn _bump_ctx_realloc(state, ptr, n){
   "Internal helper. Bump-backed realloc callback."
   if(!ptr){ return _bump_ctx_alloc(state, n) }
   0
}

fn _bump_ctx_free(state, ptr){
   "Internal helper. Bump-backed free callback (no-op)."
   if(state || ptr){ return 0 }
   0
}

fn _ensure_allocator(allocator){
   "Internal helper. Validates allocator descriptor shape."
   if(!is_list(allocator)){ panic("allocator must be a list") }
   if(len(allocator) < 5){ panic("allocator list must have at least 5 slots") }
   if(!get(allocator, 0, 0)){ panic("allocator alloc callback is missing") }
   allocator
}

fn _ensure_context(ctx){
   "Internal helper. Validates context shape."
   if(!is_list(ctx)){ panic("allocation context must be a list") }
   if(len(ctx) < 1){ panic("allocation context must expose allocator slot") }
   _ensure_allocator(get(ctx, 0, 0))
   ctx
}

fn new_allocator(alloc_fn, realloc_fn=0, free_fn=0, state=0, name="custom"){
   "Creates allocator descriptor `[alloc_fn, realloc_fn, free_fn, state, name]`."
   if(!alloc_fn){ panic("alloc_fn cannot be none") }
   return [alloc_fn, realloc_fn, free_fn, state, name]
}

fn allocator_name(allocator){
   "Returns allocator name label."
   allocator = _ensure_allocator(allocator)
   get(allocator, 4, "")
}

fn allocator_state(allocator){
   "Returns allocator state payload."
   allocator = _ensure_allocator(allocator)
   get(allocator, 3, 0)
}

fn set_allocator_state(allocator, state){
   "Updates allocator state payload."
   allocator = _ensure_allocator(allocator)
   set_idx(allocator, 3, state)
   allocator
}

fn heap_allocator(){
   "Creates a heap-backed allocator descriptor using std.core malloc/realloc/free."
   new_allocator(_heap_alloc, _heap_realloc, _heap_free, 0, "heap")
}

fn bump_allocator(cap){
   "Creates an arena-style bump allocator descriptor."
   new_allocator(_bump_ctx_alloc, _bump_ctx_realloc, _bump_ctx_free, bump_new(cap), "bump")
}

fn new_context(allocator=0){
   "Creates allocation context list `[allocator]`. Defaults to heap allocator."
   if(!allocator){ allocator = heap_allocator() }
   _ensure_allocator(allocator)
   return [allocator]
}

mut __context = new_context()

fn context(){
   "Returns process-wide default allocation context."
   __context
}

fn set_context(ctx){
   "Replaces process-wide default allocation context."
   __context = _ensure_context(ctx)
   __context
}

fn _resolve_context(ctx=0){
   "Internal helper. Resolves explicit context or global default context."
   if(!ctx){ return __context }
   _ensure_context(ctx)
}

fn context_allocator(ctx=0){
   "Returns allocator descriptor bound to context `ctx` (or global default when omitted)."
   ctx = _resolve_context(ctx)
   _ensure_allocator(get(ctx, 0, 0))
}

fn set_context_allocator(allocator, ctx=0){
   "Binds `allocator` to context `ctx` (or global default context when omitted)."
   allocator = _ensure_allocator(allocator)
   ctx = _resolve_context(ctx)
   set_idx(ctx, 0, allocator)
   ctx
}

fn with_context(ctx, thunk){
   "Runs `thunk()` with temporary process context `ctx`, then restores previous context."
   ctx = _ensure_context(ctx)
   if(!thunk){ panic("with_context requires a callable thunk") }
   def prev = __context
   __context = ctx
   defer { __context = prev }
   thunk()
}

fn ctx_alloc(n, ctx=0){
   "Allocates `n` bytes through allocator context `ctx` (or process context by default)."
   if(n <= 0){ return 0 }
   def allocator = context_allocator(ctx)
   def alloc_fn = get(allocator, 0, 0)
   alloc_fn(allocator_state(allocator), n)
}

fn ctx_realloc(ptr, n, ctx=0){
   "Reallocates `ptr` to size `n` through allocator context `ctx`."
   if(n <= 0){ return 0 }
   def allocator = context_allocator(ctx)
   def realloc_fn = get(allocator, 1, 0)
   if(!realloc_fn){
      if(!ptr){ return ctx_alloc(n, ctx) }
      return 0
   }
   realloc_fn(allocator_state(allocator), ptr, n)
}

fn ctx_free(ptr, ctx=0){
   "Frees `ptr` through allocator context `ctx`. Missing free callback is treated as no-op."
   if(!ptr){ return 0 }
   def allocator = context_allocator(ctx)
   def free_fn = get(allocator, 2, 0)
   if(!free_fn){ return 0 }
   free_fn(allocator_state(allocator), ptr)
}

fn ctx_zalloc(n, ctx=0){
   "Allocates `n` bytes and zero-fills memory through allocator context `ctx`."
   def p = ctx_alloc(n, ctx)
   if(!p){ return 0 }
   mut i = 0
   while(i < n){
      store8(p, 0, i)
      i += 1
   }
   p
}

if(comptime{__main()}){
    use std.os.time *
    use std.core.alloc *
    use std.core *

    mut __ctx_alloc_n = 0

    fn __ctx_alloc_thunk(){
       ctx_alloc(__ctx_alloc_n)
    }

    ;; Bump allocator basics.
    def state = bump_new(1024)
    assert(is_list(state), "bump state is a list")
    assert(bump_capacity(state) == 1024, "bump_capacity reports configured capacity")
    assert(bump_used(state) == 0, "fresh bump allocator has zero used bytes")
    assert(bump_available(state) == 1024, "fresh bump allocator exposes full availability")
    def m0 = bump_mark(state)
    assert(m0 == 0, "fresh bump mark is zero")
    def p1 = bump_alloc(state, 10)
    assert(p1 != 0, "first bump alloc ok")
    store8(p1, 100)
    mut p2 = bump_alloc(state, 20)
    assert(p2 == p1 + 10, "bump alloc is sequential")
    assert(bump_used(state) == 30, "bump_used tracks consumed bytes")
    assert(bump_available(state) == 994, "bump_available tracks free bytes")
    bump_reset(state)
    mut p3 = bump_alloc(state, 5)
    assert(p3 == p1, "bump reset works")
    assert(bump_used(state) == 5, "bump_reset rewinds usage")

    ;; Bump allocator marks and aligned allocation.
    def state_aligned = bump_new(64)
    def a0 = bump_alloc(state_aligned, 3)
    assert(a0 != 0, "unaligned pre-allocation should succeed")
    def m1 = bump_mark(state_aligned)
    def a1 = bump_alloc_aligned(state_aligned, 8, 16)
    assert(a1 != 0 && (a1 % 16) == 0, "bump_alloc_aligned should honor requested alignment")
    def a2 = bump_alloc(state_aligned, 4)
    assert(a2 != 0, "post-aligned allocation should still work")
    assert(bump_release(state_aligned, m1), "bump_release should accept valid marker")
    def a3 = bump_alloc_aligned(state_aligned, 8, 16)
    assert(a3 == a1, "bump_release should make aligned range reusable")
    assert(!bump_release(state_aligned, 999), "bump_release should reject invalid marker")

    ;; Bump overflow.
    def state2 = bump_new(8)
    assert(bump_alloc(state2, 10) == 0, "bump overflow returns 0")

    ;; Heap-backed context.
    def heap_ctx = new_context(heap_allocator())
    def hp = ctx_alloc(24, heap_ctx)
    assert(hp != 0, "ctx_alloc uses heap context")
    store8(hp, 77)
    assert(load8(hp) == 77, "heap context memory is writable")
    def hp2 = ctx_realloc(hp, 32, heap_ctx)
    assert(hp2 != 0, "ctx_realloc works for heap allocator")
    ctx_free(hp2, heap_ctx)

    ;; Bump-backed context.
    def arena_ctx = new_context(bump_allocator(64))
    def ap1 = ctx_alloc(8, arena_ctx)
    def ap2 = ctx_alloc(8, arena_ctx)
    assert(ap1 != 0 && ap2 == ap1 + 8, "ctx_alloc follows bump allocator state")
    assert(ctx_realloc(ap1, 16, arena_ctx) == 0,
           "bump-backed realloc is unsupported and returns 0")
    ctx_free(ap1, arena_ctx)

    ;; Reset bump state through allocator descriptor.
    def arena_state = allocator_state(context_allocator(arena_ctx))
    bump_reset(arena_state)
    def ap3 = ctx_alloc(8, arena_ctx)
    assert(ap3 == ap1, "allocator_state exposes bump state for reset")

    ;; Default/global context switching.
    def prev_ctx = context()
    set_context(arena_ctx)
    def gp1 = ctx_alloc(4)
    def gp2 = ctx_alloc(4)
    assert(gp2 == gp1 + 4, "global context routes implicit ctx_alloc")
    set_context(prev_ctx)

    ;; set_context_allocator updates existing context.
    def swap_ctx = new_context()
    set_context_allocator(bump_allocator(32), swap_ctx)
    def sp1 = ctx_alloc(8, swap_ctx)
    def sp2 = ctx_alloc(8, swap_ctx)
    assert(sp2 == sp1 + 8, "set_context_allocator swaps allocator in-place")

    ;; Scoped context switching.
    __ctx_alloc_n = 5
    def wp1 = with_context(arena_ctx, __ctx_alloc_thunk)
    __ctx_alloc_n = 5
    def wp2 = with_context(arena_ctx, __ctx_alloc_thunk)
    assert(wp2 == wp1 + 5, "with_context temporarily switches allocator context")
    assert(context() == prev_ctx, "with_context restores previous context")

    ;; Zeroed allocation.
    def zp = ctx_zalloc(8, heap_ctx)
    assert(zp != 0, "ctx_zalloc allocates memory")
    assert(load8(zp, 0) == 0 && load8(zp, 7) == 0, "ctx_zalloc zero-fills memory")
    ctx_free(zp, heap_ctx)

    print("✓ std.core.alloc tests passed")
}
