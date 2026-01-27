#!/bin/ny
;; Raylib FFI (Example) - LowLevel ffi calls

use std.os.ffi *
use std.str.io *

def h = dlopen("/usr/lib/libraylib.so", 2)
if(h != 0){
   def InitWindow        = dlsym(h, "InitWindow")
   def WindowShouldClose = dlsym(h, "WindowShouldClose")
   def BeginDrawing      = dlsym(h, "BeginDrawing")
   def EndDrawing        = dlsym(h, "EndDrawing")
   def ClearBackground   = dlsym(h, "ClearBackground")
   def CloseWindow       = dlsym(h, "CloseWindow")
   call3_void(InitWindow, 800, 450, "Nytrix")
   while(call0(WindowShouldClose) == 0){
      call0_void(BeginDrawing)
      call1_void(ClearBackground, 0xFF181818)
      call0_void(EndDrawing)
   }
   call0_void(CloseWindow)
   dlclose(h)
   print("✓ Raylib window closed")
} else {
   print("[/usr/lib/libraylib.so] Raylib not found")
}
