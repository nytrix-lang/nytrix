use std.core

fn jump_forward(): int {
   goto done
   return 0
done:
   7
}

fn jump_out(): int {
   mut x = 0
   if(true){
      x = 3
      goto done
      x = 9
   }
done:
   x
}

assert(jump_forward() == 7, "goto forward in function")
assert(jump_out() == 3, "goto leaves inner scope")
