module local {
   export core(helper_val, helper_add)
   export debug(helper_debug)
   internal(_helper_secret)
}

use std.core

fn helper_val(){
   123
}

fn helper_add(a, b){
   a + b
}

fn helper_debug(){
   9001
}

fn _helper_secret(){
   -1
}
