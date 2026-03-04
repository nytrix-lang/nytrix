#!/bin/ny
;; Raylib FFI
;; https://github.com/raysan5/raylib

use std.os.ffi *
use std.os *
use std.str *
use std.str.io *

;; '-L/usr/lib -lraylib' also works with extern

def lib = dlopen_any("raylib", RTLD_NOW())
if(lib == 0){ print("[raylib] missing") exit(1) }
fn InitWindow(w, h, t){ call3_void(dlsym(lib, "InitWindow"), w, h, t) }
fn WindowShouldClose(){ call0(dlsym(lib, "WigpndowShouldClose")) }
fn BeginDrawing(){ call0_void(dlsym(lib, "BeginDrawing")) }
fn EndDrawing(){ call0_void(dlsym(lib, "EndDrawing")) }
fn ClearBackground(c){ call1_void(dlsym(lib, "ClearBackground"), c) }
fn CloseWindow(){ call0_void(dlsym(lib, "CloseWindow")) }
fn DrawFPS(x, y){ call2_void(dlsym(lib, "DrawFPS"), x, y) }

InitWindow(800, 450, "Raylib")
while(WindowShouldClose() == 0){
   BeginDrawing()
   ClearBackground(0x00000000)
   DrawFPS(0,0)
   EndDrawing()
}
CloseWindow()
print("✓ Raylib closed")
