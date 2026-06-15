;; expect: duplicate impl method 'Meter.val/1'
use std.core

impl Meter {
   fn val(self m) int {
      m.get("value", 0)
   }
}

impl Meter {
   fn val(self m) int {
      m.get("other", 0)
   }
}

def Meter m = Meter({"value": 1})
print(m.val)
