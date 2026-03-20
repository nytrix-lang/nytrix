use std.core

fn auto_numeric_sum(n){
   mut i = 0
   mut acc = 0
   while(i < n){
      acc += i
      i += 1
   }
   acc
}

fn auto_numeric_for(n){
   mut acc = 0
   for(mut i = 0 i < n ++i){
      acc += i * 2
   }
   acc
}

fn auto_typed_buffer_sum(n){
   def p = malloc(n)
   mut i = 0
   while(i < n){
      store8(p, i, i)
      i += 1
   }
   mut j = 0
   mut acc = 0
   while(j < n){
      acc += load8(p, j)
      j += 1
   }
   free(p)
   acc
}

assert(auto_numeric_sum(10) == 45, "auto SIMD numeric while")
assert(auto_numeric_for(5) == 20, "auto SIMD numeric for")
assert(auto_typed_buffer_sum(8) == 28, "auto SIMD typed buffer loop")
print("auto SIMD tests passed")
