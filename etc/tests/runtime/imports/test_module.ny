use std.core

;; Local module definition (Test)

module test_module (
   local_add,
   local_greet
)

fn local_add(x, y){
   x + y
}

fn local_greet(name){
   "Hello, " + name + " from local module!"
}
