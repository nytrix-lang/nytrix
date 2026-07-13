use std.core *
mut xs = [1, 2, 3]
mut int: i = 0
while(i < 8){
  xs = set_idx(xs, i % 3, get(xs, i % 3, 0) + i)
  i += 1
}
get(xs, 0, 0)
