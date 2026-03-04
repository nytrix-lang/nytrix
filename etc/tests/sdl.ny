#!/bin/ny
;; SDL3
;; https://github.com/libsdl-org/SDL

#include <SDL3/SDL.h>

def window = SDL_CreateWindow("Hello", 640, 480, 0)
def renderer = SDL_CreateRenderer(window, 0)
while(1){
   SDL_SetRenderDrawColor(renderer, 33, 33, 33, 255)
   SDL_RenderClear(renderer)
   SDL_RenderPresent(renderer)
   SDL_Delay(16)
}
SDL_DestroyRenderer(renderer)
SDL_DestroyWindow(window)
SDL_Quit()
