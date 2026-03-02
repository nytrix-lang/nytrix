;; Keywords: util utils
;; General utility module.

module std.util.utils (
   parse_ast,
   counter, counter_add, most_common
)
use std.util.ast as ast
use std.util.counter as ctr

fn parse_ast(source){
   "Parses Nytrix source code and returns the AST as a nested structure (list of dicts)."
   ast.parse_ast(source)
}

fn counter(xs){
   "Creates a frequency counter dictionary from the elements of list or string `xs`."
   ctr.counter(xs)
}

fn counter_add(d, key, n){
   "Adds `n` to the count of `key` in counter dictionary `d`."
   ctr.counter_add(d, key, n)
}

fn most_common(d){
   "Returns a list of `[item, count]` pairs from counter `d`, sorted by count in descending order."
   ctr.most_common(d)
}

if(comptime{__main()}){
    use std.util.utils *
    use std.core *
    use std.core.error *
    use std.core.dict *

    print("Testing std.util.utils...")

    mut c = counter(["a", "b", "a", "c"])
    assert(dict_get(c, "a", 0) == 2, "counter a")
    assert(dict_get(c, "b", 0) == 1, "counter b")

    c = counter_add(c, "b", 4)
    assert(dict_get(c, "b", 0) == 5, "counter_add")

    def mc = most_common(c)
    assert(len(mc) >= 2, "most_common len")
    assert(get(get(mc, 0), 1) >= get(get(mc, 1), 1), "most_common sorted desc")

    print("✓ std.util.utils tests passed")
}
