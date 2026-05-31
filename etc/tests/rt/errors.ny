use std.core
use std.core.error
use std.core.iter as it
use std.core.str

mut caught = ""

fn id(v){
   v
}

fn any_id(any: v): any {
   v
}

fn capture(thunk){
   caught = ""
   try {
      thunk()
   } catch e {
      caught = e
   }
   caught
}

fn did_catch(thunk){
   try {
      thunk()
      false
   } catch _ {
      true
   }
}

fn flatten_inline(l){
   is_list(l) ? mapcat(flatten_inline, l) : [l]
}

fn flatten_try(x){
   if(is_str(x)) return [x]
   try{
      mapcat(flatten_try, x)
   } catch(_){[x]}
}

fn flatten_recursive(x){
   if(is_str(x) || !is_list(x)){ return [x] }
   it.mapcat(flatten_recursive, x)
}

fn catch_len(x){
   try{
      x.len
   } catch(_){
      -1
   }
}

fn fdiv_zero(f64: a, f64: b){
   a / b
}

assert(flatten_inline([1, [2, [3, 4, [1, 2, 3, "ny"]], 5]]) == [1, 2, 3, 4, 1, 2, 3, "ny", 5],
"inline function-body flatten should preserve scalar leaves")
assert(flatten_try([1, [2, [3, 4, [1, 2, 3, "ny"]], 5]]) == [1, 2, 3, 4, 1, 2, 3, "ny", 5],
"try/catch flatten should recover scalar leaves")
assert(flatten_recursive([1, [2, [3, 4, [1, 2, 3, "ny"]], 5]]) == [1, 2, 3, 4, 1, 2, 3, "ny", 5],
"recursive flatten should preserve scalar leaves")
assert(catch_len("abc") == 3, "try/catch len should pass through valid sequence")
assert(catch_len(1) == -1, "try/catch len should recover from invalid scalar input")
assert(str_contains(capture(fn(){ len(id(1)) }), "len expects"), "len should reject ints")
assert(str_contains(capture(fn(){ len(id(1)) }), "got int"), "len should report bad value")
assert(str_contains(capture(fn(){ contains(id(1), 2) }), "contains expects"), "receiver contains should reject ints")
assert(str_contains(capture(fn(){ get(id(1), 0) }), "get expects"), "receiver get should reject ints")
assert(str_contains(capture(fn(){ return append(id(1), 2) }), "append expects"), "receiver append should reject non-lists")
assert(str_contains(capture(fn(){ pop(id(1)) }), "pop expects"), "receiver pop should reject non-lists")
assert(str_contains(capture(fn(){ slice(id(1), 0, 1) }), "slice expects"), "slice should reject ints")
assert(str_contains(capture(fn(){ set([1], 9, 2) }), "set index out of range"), "receiver set should reject out-of-range writes")
assert(str_contains(capture(fn(){ set([1], 9, 2) }), "index=9"), "receiver set should report bad index")
assert(str_contains(capture(fn(){ id(1) / id(0) }), "division by zero"), "integer division by zero should panic")
assert(str_contains(capture(fn(){ id(1) % id(0) }), "modulo by zero"), "integer modulo by zero should panic")
assert(str_contains(capture(fn(){ fdiv_zero(1.0, 0.0) }), "division by zero"), "float division by zero should panic")
def div_err = exception(ERR_DIV_ZERO, "division by zero")
assert(get(div_err, "kind", "") == ERR_DIV_ZERO, "structured div error .get kind")
assert(get(div_err, "message", "") == "division by zero", "structured div error .get message")
assert(error_kind(div_err) == ERR_DIV_ZERO, "structured div error kind")
assert(error_message(div_err) == "division by zero", "structured div error message")
assert(is_error(div_err, ERR_DIV_ZERO), "structured div error match")
def runtime_warn = warning(WARN_RUNTIME, "slow fallback")
assert(error_kind(runtime_warn) == WARN_RUNTIME, "structured warning kind")
assert(did_catch(fn(){ len(id(1)) }), "len should reject ints")
assert(did_catch(fn(){ contains(id(1), 2) }), "receiver contains should reject ints")
assert(did_catch(fn(){ get(id(1), 0) }), "receiver get should reject ints")
assert(did_catch(fn(){ return append(id(1), 2) }), "receiver append should reject non-lists")
assert(did_catch(fn(){ pop(id(1)) }), "receiver pop should reject non-lists")
assert(did_catch(fn(){ slice(id(1), 0, 1) }), "slice should reject ints")
assert(did_catch(fn(){ set([1], 9, 2) }), "receiver set should reject out-of-range writes")
assert(did_catch(fn(){ id(1) / id(0) }), "division by zero should be catchable")
assert(did_catch(fn(){ it.mapcat(fn(v){ [v] }, any_id(1)) }),
"mapcat should reject non-sequences")
assert(did_catch(fn(){ it.any(any_id(1), fn(v){ v }) }),
"any should reject non-sequences")
assert(get(any_id(nil), 0, 55) == 55, "receiver get should return default for nil")
assert(set_idx(any_id(nil), 0, 1) == 0, "receiver set_idx should reject nil")
def raw_probe = malloc(8)
assert(get(any_id(raw_probe), 0, 77) == 77, "receiver get should return default for raw pointers")
assert(set_idx(any_id(raw_probe), 0, 1) == 0, "receiver set_idx should reject raw pointers")
free(raw_probe)
def xs = [1]
assert(get(xs, 0) == 1, "receiver get valid list")
assert(append(xs, 2) == [1, 2], "receiver append valid list")
mut d = dict(2)
d = set(d, "fit_scale", 49.0)
def nested = [d, 1]
assert(get(get(nested, 0, 0), "fit_scale", 0.0) == 49.0, "chained receiver get should preserve dict")
assert(slice("abcd", 1, 3) == "bc", "slice valid string")
print("✓ runtime error/try syntax tests passed")
