;; Keywords: cache memoization
;; Shared cache operations for hot-path dictionaries and memoized lookups.
module std.core.cache(cache_reset_if_over, cache_put_reset)
use std.core

fn _cache_reset_cap(int: reset_cap, int: max_items): int {
   "Chooses the capacity for a replacement cache dictionary."
   if(reset_cap > 0){ return reset_cap }
   if(max_items > 0){ return max_items }
   8
}

fn cache_reset_if_over(any: c, int: max_items, int: reset_cap=0): dict {
   "Returns a fresh dict when `c` exceeds `max_items`; otherwise returns `c`."
   if(!is_dict(c)){ return dict(_cache_reset_cap(reset_cap, max_items)) }
   if(max_items > 0 && c.len > max_items){ return dict(_cache_reset_cap(reset_cap, max_items)) }
   c
}

fn cache_put_reset(any: c, any: key, any: value, int: max_items, int: reset_cap=0): dict {
   "Stores `key` and resets the cache when it grows past `max_items`."
   mut out = cache_reset_if_over(c, max_items, reset_cap)
   out[key] = value
   cache_reset_if_over(out, max_items, reset_cap)
}
