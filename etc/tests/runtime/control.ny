use std.core *
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.str.io *
use std.str *

;; Runtime control flow + break + paren-less syntax (Test)

mut i = 0
while(i < 10){
   if(i == 5){ break }
   i = i + 1
}
assert(i == 5, "simple break")

i = 0
mut flag = 0
while(i < 10){
   if(i == 5){
      flag = 1
      break
   }
   i = i + 1
}
assert(flag == 1, "update before break")
assert(i == 5, "break stops loop")

i = 0
mut j = 0
mut count = 0
while(i < 3){
   j = 0
   while(j < 3){
      if(j == 1){ break }
      count = count + 1
      j = j + 1
   }
   i = i + 1
}
assert(count == 3, "nested break inner only")

def m = 1
mut matches = 0
i = 0
while(i < 6){
   mut is_match = 1
   j = 0
   while(j < 1){
      mut char_match = 0
      if(i == 1){ char_match = 1 }
      if(i == 3){ char_match = 1 }
      if(i == 5){ char_match = 1 }
      if(char_match == 0){
         is_match = 0
         break
      }
      j = j + 1
   }
   if(is_match == 1){
      matches = matches + 1
      i = i + m
   } else {
      i = i + 1
   }
}
assert(matches == 3, "break + assignment repro")

if(0){ panic("if(0)") } else { assert(1, "if else") }
if(1){ assert(1, "if true") } else { panic("if else") }

if 0 { panic("if 0") } else { assert(1, "if 0 else") }
if 1 { assert(1, "if 1") } else { panic("if 1 else") }

i = 0
while(i < 3){ i = i + 1 }
assert(i == 3, "while paren")

i = 0
while i < 3 { i = i + 1 }
assert(i == 3, "while noparen")

mut s = 0
for(x in [1,2,3]){ s = s + x }
assert(s == 6, "for paren")

s = 0
for x in [1,2,3] { s = s + x }
assert(s == 6, "for noparen")

print("✓ runtime control flow tests passed")

