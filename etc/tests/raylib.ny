#!/bin/ny
;; Raylib
;; https://github.com/raysan5/raylib

#include <raylib.h>

InitWindow(800, 450, "Raylib")
while(WindowShouldClose() == 0){
   BeginDrawing()
   ClearBackground(0x00000000)
   DrawFPS(0,0)
   EndDrawing()
}
CloseWindow()
