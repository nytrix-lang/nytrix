;; Keywords: core list
;; Compatibility shim for std.core.list

module std.core.list (
   list, is_list, list_clone, list_clear, append, pop, extend, sort,
   len, get, set_idx, load_item, store_item, slice, to_str
)
use std.core *
