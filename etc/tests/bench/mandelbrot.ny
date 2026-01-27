use std.io
use std.os.time

;; Mandelbrot Set (Benchmark)
;; Strength reduction, loop unrolling, and minimal dispatch.

fn mandelbrot(w, h, max_iter){
   def count = 0
   def sx = 4.0 / w
   def sy = 4.0 / h
   def hw_sx = - (w * 0.6666667) * sx
   def hh_sy = - (h * 0.5) * sy
   def four = 4.0

   def c_im = hh_sy
   def y = 0
   while(y < h){
      def c_re = hw_sx
      def x = 0
      while(x < w){
         def zr = 0.0
         def zi = 0.0
         def zr2 = 0.0
         def zi2 = 0.0
         def i = 0
         
         while(i < max_iter){
            zr2 = zr * zr
            zi2 = zi * zi
            if(zr2 + zi2 > four){ break }
            zi = (zr + zr) * zi + c_im
            zr = zr2 - zi2 + c_re
            i += 1
            
            if(i == max_iter){ break }
            zr2 = zr * zr
            zi2 = zi * zi
            if(zr2 + zi2 > four){ break }
            zi = (zr + zr) * zi + c_im
            zr = zr2 - zi2 + c_re
            i += 1

            if(i == max_iter){ break }
            zr2 = zr * zr
            zi2 = zi * zi
            if(zr2 + zi2 > four){ break }
            zi = (zr + zr) * zi + c_im
            zr = zr2 - zi2 + c_re
            i += 1

            if(i == max_iter){ break }
            zr2 = zr * zr
            zi2 = zi * zi
            if(zr2 + zi2 > four){ break }
            zi = (zr + zr) * zi + c_im
            zr = zr2 - zi2 + c_re
            i += 1
         }

         if(i == max_iter){ count += 1 }
         c_re += sx
         x += 1
      }
      c_im += sy
      y += 1
   }
   count
}

def size = 175
def WIDTH = size
def HEIGHT = size
def ITERS = 72

print("Mandelbrot:", WIDTH, "x", HEIGHT, "iters:", ITERS)

def t0 = ticks()
def res = mandelbrot(WIDTH, HEIGHT, ITERS)
def t1 = ticks()

print("Points:", res)
print("Time:", (t1 - t0) / 1000000, "ms")
