;; Keywords: util general
;; General utility module.

module std.util.general (
   uuid
)
use std.core *
use std.math.random *
use std.str.io *

fn uuid(){
   "Generates a random Version 4 UUID string (e.g., 'f47ac10b-58cc-4372-a567-0e02b2c3d479')."
   def out = malloc(64)
   init_str(out, 36)
   mut i=0  mut o=0
   mut r=0 mut b=0
   mut v1=0 mut c1=255
   mut v2=0 mut c2=255
   while(i<16){
      r = rand()
      b = r % 256
      if(i==6){ b = (b % 16) + 64  }
      if(i==8){ b = (b % 64) + 128  }
      v1 = (b / 16) % 16
      if(v1 < 10){ c1 = 48 + v1 } else { c1 = 87 + v1 }
      v2 = b % 16
      if(v2 < 10){ c2 = 48 + v2 } else { c2 = 87 + v2 }
      store8(out, c1, o)
      store8(out, c2, o + 1)
      o = o + 2
      if(i==3 || i==5 || i==7 || i==9){ store8(out, 45, o)  o=o+1  }
      i=i+1
   }
   store8(out, 0, o)
   ; length set at init
   return out
}