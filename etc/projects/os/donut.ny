#!/usr/bin/env ny

;; Keywords: cli terminal donut example
;                 @@@@@@@@@@
;            $$$$$$$########$$$$$
;         $$$####*****!!!!!****####
;       #####****!!*!!!!!=!!!!*!****#
;     *###*****!!!===;;;;;;;====!!!!!**
;    *****!!*!!==;:::~~~~~~~~::;;==!!!!!
;   !***!*!!!==;::~-,,.....,,-~~:;;===!!=
;  !!!!!!!!==;;~~-,...........,--::;===!=
;  !!!!!!!==;::~-......   ......-~:;;;===;
;  =!!!!===;;:~-,...         ...,-~::;;===;
;  ==!!===;;::~-,..           .,-~:;;;===;;
;  ;=======;;:~~-.,          :;;::;;=====;:
;  ;;=======;;;:::::       ;!***!!======;:
;  ~;;;===========!!***#$$$$$##***!!!==;~
;   ~:;;=======!!!**##$$@@@@$$$#**!!!=;~
;    -::;;=====!!!**##$$$$$$###**!!=;;-
;     ,~:;;;;==!!!!***######*****!=;~.
;       ,-~::;;===!!!*!!!!!!*!!=;:~.
;         .-~~:;:;;===!!!!==;;:~,
;            ..,--~~~~~~~~--,.
use std.core.term,std.math

def P=6.28 C=".,-~:;=!*#$@"
fn L(s){mut r=[] mut i=0.0 while i<P{r=r.append([sin(i),cos(i)]) i+=s}r}
fn D(c,z,A,B,w,h,x,y){
 mut i=0 while i<z.len{z[i]=0.0 i+=1} canvas_clear(c)
 def sx,cx,sy,cy=sin(x),cos(x),sin(y),cos(y)
 def ox,oy,kx,ky=w/2,h/2,w*0.38,h*0.72
 for t in A{
  def st,ct,r=t[0],t[1],t[1]+2.0
  for u in B{
   def sp,cp,d=u[0],u[1],1.0/(5.0+cx*r*u[0]+st*sx)
   def X=int(ox+kx*d*(r*(cy*cp+sx*sy*sp)-st*cx*sy))
   def Y=int(oy-ky*d*(r*(sy*cp-sx*cy*sp)+st*cx*cy)) o=X+Y*w
   if X>0&&X<w&&Y>0&&Y<h&&d>z[o]{
    def l=cp*ct*sy-cx*ct*sp-sx*st+cy*(cx*st-ct*sx*sp) I=int(l*8.0)
    if l>0.0{z[o]=d canvas_set(c,X,Y,C[I<0?0:I>11?11:I],6,0)}
   }
  }
 }
 canvas_refresh(c)
}

def A,B=L(0.07),L(0.02)
mut x,y=0.0,0.0
tui_canvas_loop(fn(c,z,w,h){D(c,z,A,B,w,h,x,y) x+=0.04 y+=0.02})
