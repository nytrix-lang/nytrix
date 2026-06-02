use std.core

#include <stdlib.h> as ""

;; Builtin-shaped user functions must shadow compiler fast paths without
;; hiding unrelated unprefixed C imports.
fn len(x){
   return 99
}

fn int(x){
   return 123
}

fn get(a, b){
   return 88
}

fn set_idx(a, b, c){
   return 77
}

fn set(a, b, c){
   return 66
}

fn runtime_tag_raw(x){
   return 55
}

assert(len([1, 2, 3]) == 99, "user len shadows fast len")
assert(int("7") == 123, "user int shadows fast int cast")
assert(get([1, 2, 3], 1) == 88, "user get shadows fast get")
assert(set_idx([1, 2, 3], 1, 9) == 77, "user set_idx shadows fast set_idx")
assert(set([1, 2, 3], 1, 9) == 66, "user set shadows fast set")
assert(runtime_tag_raw("list") == 55, "user runtime_tag_raw shadows fast tag")

assert(atoi("42") == 42, "unprefixed C import stays callable under shadowing")
assert(atof("6.5") > 6.0, "unprefixed C float import stays callable under shadowing")
