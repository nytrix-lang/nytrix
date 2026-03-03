use std.core *
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.str.io *
use std.str *
use std.os *
use std.os.time *

;; Mandelbrot Set (Benchmark)

fn _bench_scale_percent(){
   def raw = env("NYTRIX_BENCH_SCALE")
   if(is_str(raw) && str_len(raw) > 0){
      def v = atoi(raw)
      if(v > 0){ return v }
   }
   100
}

fn _bench_scale(val, minv){
   mut out = (val * _bench_scale_percent()) / 100
   if(out < minv){ out = minv }
   out
}

fn mandelbrot(w, h, max_iter){
   mut count = 0
   def sx = 4.0 / w
   def sy = 4.0 / h
   def hw_sx = - (w * 0.6666667) * sx
   def hh_sy = - (h * 0.5) * sy
   def four = 4.0

   mut c_im = hh_sy
   mut y = 0
   while(y < h){
      mut c_re = hw_sx
      mut x = 0
      while(x < w){
         mut zr = 0.0
         mut zi = 0.0
         mut zr2 = 0.0
         mut zi2 = 0.0
         mut i = 0

         while(i < max_iter){
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

def size = _bench_scale(80, 20)
def WIDTH = size
def HEIGHT = size
def ITERS = _bench_scale(64, 8)

print("Mandelbrot:", WIDTH, "x", HEIGHT, "iters:", ITERS)

def t0 = ticks()
def res = mandelbrot(WIDTH, HEIGHT, ITERS)
def t1 = ticks()

print("Points:", res)
print("Time:", (t1 - t0) / 1000000, "ms")
