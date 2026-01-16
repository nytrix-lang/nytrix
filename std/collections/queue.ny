;; Keywords: collections queue
;; Collections Queue module.

module std.collections.queue (
   queue, queue_push, queue_pop, queue_len
)

fn _queue_alloc(cap){
   "Internal: allocate a queue with capacity cap."
   if(cap < 8){ cap = 8 }
   def p = __malloc(32 + cap * 8)
   store64(p, 106, -8)
   store64(p, 0, 0)   ; len
   store64(p, cap, 8) ; cap
   store64(p, 0, 16)  ; head
   store64(p, 0, 24)  ; tail
   p
}

fn queue(){
   "Creates a new FIFO **queue**."
   _queue_alloc(8)
}

fn _queue_cap(q){
   "Internal: return queue capacity."
   load64(q, 8)
}

fn _queue_len(q){
   "Internal: return queue length."
   load64(q, 0)
}

fn _queue_head(q){
   "Internal: return queue head index."
   load64(q, 16)
}

fn _queue_tail(q){
   "Internal: return queue tail index."
   load64(q, 24)
}

fn _queue_get(q, i){
   "Internal: load item at ring index i."
   load64(q, 32 + i * 8)
}

fn _queue_set(q, i, v){
   "Internal: store item at ring index i."
   store64(q, v, 32 + i * 8)
}

fn _queue_grow(q){
   "Internal: grow queue capacity."
   def cap = _queue_cap(q)
   def new_cap = cap * 2
   def out = _queue_alloc(new_cap)
   def n = _queue_len(q)
   def h = _queue_head(q)
   def i = 0
   while(i < n){
      def idx = h + i
      if(idx >= cap){ idx -= cap }
      _queue_set(out, i, _queue_get(q, idx))
      i += 1
   }
   store64(out, n, 0)
   store64(out, 0, 16)
   store64(out, n, 24)
   __free(q)
   out
}

fn queue_push(q, v){
   "Pushes value `v` to the back of queue `q`."
   if(!is_ptr(q) || load64(q, -8) != 106){ q }
   else {
      def n = _queue_len(q)  def cap = _queue_cap(q)
      if(n >= cap){ q = _queue_grow(q)  cap = _queue_cap(q) }
      def t = _queue_tail(q)
      _queue_set(q, t, v)
      def next_t = t + 1
      if(next_t >= cap){ next_t = 0 }
      store64(q, next_t, 24)
      store64(q, n + 1, 0)
      q
   }
}

fn queue_pop(q){
   "Removes and returns the front element of queue `q`."
   if(!is_ptr(q) || load64(q, -8) != 106){ 0 }
   else {
      def n = _queue_len(q)
      if(n == 0){ 0 }
      else {
         def h = _queue_head(q)  def v = _queue_get(q, h)
         def next_h = h + 1
         if(next_h >= _queue_cap(q)){ next_h = 0 }
         store64(q, next_h, 16)
         store64(q, n - 1, 0)
         v
      }
   }
}

fn queue_len(q){
   "Returns the number of elements in queue `q`."
   if(!is_ptr(q) || load64(q, -8) != 106){ 0 } else { _queue_len(q) }
}