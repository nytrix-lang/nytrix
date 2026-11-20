#!/usr/bin/env ny
;; Keywords: cli terminal donut
;; Donut - https://www.a1k0n.net/2006/09/15/obfuscated-c-donut.html

              use core;=========--
         use core.term mut A,B=.55;----
      ,.35 fn R(u,v,a,b,c,d,L,M,N){g=4.41-
    u*u-v*v if g<=0{return}mut t=sqrt(g)+.01
  for _ in 0..56{p=c*u+d*t x=d*u-c*t y=b*v+a*;
 p z=b*p-a*v r=sqrt(x*x+y*y)q=r-1.48 e=sqrt(q*;
q+z*z)if e<.632{l=q*(x*L+y*M)+z*N*r return l<.2*
e*r?1:l<.65*e*r?2:            3}t-=e-.62}0}fn D(
any c,z,w,h)int                  {def i,j=int(;.
w),int(h)a,b,                    d,e=sin(A),cos(
A),sin(B),cos(B)z=            .65*e-.45*d L,M,N=
-.45*e-.65*d,.65*b+z*a,z*b-.65*a mut f64 s=min(;
 i+1,j*2+1)s*=.18 v=(j-1)/s for y in 0..j{u=;..
  (1-i)*.5/s for x in 0..i{k=R(u,v,a,b,d,e,L,M
    ,N)canvas_set(c,x,y," ░▒▓"[k],[0,4,5,7][
      k],k/3)u+=1/s}v-=2/s}canvas_refresh(
         c)A+=.035 B+=.021;-...........
              0}tui_canvas_loop(D)
