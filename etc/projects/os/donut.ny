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

fn draw(C,z,T,R,w,h,a,b){
 mut i=0 while i<z.len{z[i]=0.0 i+=1} canvas_clear(C)
 def A,B,Cs,D=sin(a),cos(a),sin(b),cos(b)
 def O,P,K,L=w/2,h/2,w*0.38,h*0.72
 def E,F=D*B-A,B+D*A
 for t in T{def s,c=t[0],t[1] for p in R {
  def u,v=p[0],p[1]
  def n,m=c*v,c*u
  def X,Y=n+2.0*v,m+2.0*u
  def U,V=A*Y-B*s,B*Y+A*s
  def q=1.0/(5.0+V)
  def x,y=int(O+K*q*(D*X+Cs*U)),int(P-L*q*(Cs*X-D*U))
  if x>0&&x<w&&y>0&&y<h {def o=x+y*w if q>z[o] {
   def l=Cs*n+s*E-m*F
   if l>0.0{z[o]=q def _=canvas_set(C,x,y,".,-~:;=!*#$@"[max(0,min(11,int(l*8.0)))],6,0)}
  }}
 }}
 canvas_refresh(C)
}

mut rx,ry = 0.0,0.0
fn sweep(s)(0..int(TAU/s+1)).filter(fn(i){i*s<TAU}).map(fn(i){[sin(i*s),cos(i*s)]})
tui_canvas_loop(fn(c, depth, w, h) {
  draw(c, depth, sweep(0.07), sweep(0.02), w, h, rx, ry)
  rx += 0.04
  ry += 0.02
})
