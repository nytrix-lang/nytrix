;; expect: variable declaration: expected str, got int
use std.core

fn pick(rows){
   rows[1][0]
}

def str: s = pick([[1], [2]])
print(s)
