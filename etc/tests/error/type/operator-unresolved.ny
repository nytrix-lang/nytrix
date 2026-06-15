;; expect: unresolved operator '+' for Meter and Meter
use std.core

impl Meter {}

def Meter a = Meter({"value": 1})
def Meter b = Meter({"value": 2})
def c = a + b
print(c)
