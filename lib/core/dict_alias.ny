;; Keywords: core dict
;; Compatibility shim for std.core.dict

module std.core.dict (
   dict, dict_len, dict_get, dict_has, dict_set, dict_del, dict_clone,
   dict_merge, dict_items, dict_keys, dict_values
)
use std.core.dict_mod *
