#!/bin/ny
;; Raylib FFI (Example)
;; https://github.com/raysan5/raylib

use std.os.ffi *
use std.os *
use std.str *
use std.str.io *

; '-L/usr/lib -lraylib' also works with extern

def is_linux = eq(__os_name(), "linux")
def display = env("DISPLAY")
def wayland = env("WAYLAND_DISPLAY")
def headless = is_linux && !display && !wayland

if(headless){
   print("[raylib] headless linux session; skipping window demo")
} else {
   def h = dlopen_any("raylib", RTLD_NOW())
   if(h != 0){
      def InitWindowP        = dlsym(h, "InitWindow")
      def WindowShouldCloseP = dlsym(h, "WindowShouldClose")
      def BeginDrawingP      = dlsym(h, "BeginDrawing")
      def EndDrawingP        = dlsym(h, "EndDrawing")
      def ClearBackgroundP   = dlsym(h, "ClearBackground")
      def CloseWindowP       = dlsym(h, "CloseWindow")
      def SetTargetFPSP      = dlsym(h, "SetTargetFPS")
      if(InitWindowP == 0 || WindowShouldCloseP == 0 || BeginDrawingP == 0 ||
         EndDrawingP == 0 || ClearBackgroundP == 0 || CloseWindowP == 0){
         print("raylib symbols missing in loaded library")
      } else {
         call3_void(InitWindowP, 800, 450, "Nytrix")
         if(SetTargetFPSP != 0){ call1_void(SetTargetFPSP, 120) }

         mut frame_limit = atoi(env("NYTRIX_RAYLIB_FRAMES"))
         if(frame_limit < 0){ frame_limit = 0 }
         mut frame_count = 0

         while(call0(WindowShouldCloseP) == 0){
            call0_void(BeginDrawingP)
            call1_void(ClearBackgroundP, 0xFF181818)
            call0_void(EndDrawingP)

            frame_count += 1
            if(frame_limit > 0 && frame_count >= frame_limit){ break }
         }
         call0_void(CloseWindowP)
         print("✓ Raylib window closed")
      }
      dlclose(h)
   } else {
      print("[raylib] library not found")
   }
}
