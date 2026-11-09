#!/usr/bin/env ny
;; Keywords: cli terminal donut
;; Donut - https://www.a1k0n.net/2006/09/15/obfuscated-c-donut.html

                                  use core ;#########.....
                           use core.os (exit) ;###############...
                       use core.os.fixture as fixture ;#########***..
                   use core.term ;###########################**********..
                mut f64 A=.55 mut f64 B=.35 ;##############****************.
             fn R(f64 u,f64 v,f64 a,f64 b,f64 c,f64 d,f64 L,f64 M,f64 N)int{ ;.
           def f64 g=u*u+v*v if g>=4.41{return 0} ;###**********************++++.
         def f64 X=d*u def f64 Y=b*v+a*c*u def f64 Z=b*c*u-a*v ;**********++++++++.
        mut f64 t=float(sqrt(4.41-g))+.01 for _ in 0..56{ if t<=-2.2{return 0} ;+++.
      def f64 x=X-c*t def f64 y=Y+a*d*t def f64 z=Z+b*d*t ;***********+++++++++++++++.
     def f64 r=float(sqrt(x*x+y*y)) def f64 q=r-1.48 def f64 e=float(sqrt(q*q+z*z)) ;+.
    def f64 f=e-.62 if f<.012{ def                      f64 l=q*(x*L+y*M)+z*N*r ;+++++=.
   def f64 m=e*r return                                    l<.2*m?1:l<.65*m?2:3 } ;+====.
   t-=f } 0 } ;##############..                              ..+++++++++++++++++++======.
  fn D(any c,int w,int h)int{                                 if w<1||h<1{return 0} ;====.
  def int top=w>=18&&h>=9?1:0                                 def ok=w>=8&&h>=6 ;========.
  def f64 a=float(sin(A)) def                                 f64 b=float(cos(A)) ;======.
  def f64 d=float(sin(B)) def                               f64 e=float(cos(B)) ;========.
  def f64 z=.65*e-.45*d def f64                           L=-.45*e-.65*d ;===============.
  def f64 M=.65*b+z*a def f64                         N=z*b-.65*a ;===================:::.
   def f64 cx=float(w-1)*.5 def f64 cy=float(h+top-1)*.5 mut f64 s=float(w-4) ;=====::::.
   def f64 sy=float(h-top-4) if sy*2.0<s{s=sy*2.0} s*=.9/4.2 if !ok{s=1.0} ;=====:::::::.
    def f64 inv=1.0/s def f64 dy=2.0*inv mut f64 v=cy*dy for y in 0..h{ ;=====:::::::::.
     mut f64 u=-cx*inv for x in 0..w{ def int k=ok&&y>=top?R(u,v,a,b,d,e,L,M,N):0 ;:::.
      canvas_set(c,x,y,k==0?" ":k==1?"░":k==2?"▒":"▓", k==0?0:k==3?7:k+3, k==3?1:0) ;.
        u+=inv } v-=dy } if !ok{canvas_set(c,int(w/2),int(h/2),"·",7,1)} ;:::::::::.
         if top>0{canvas_print(c,0,0,"q/esc · Donut",6,1)} canvas_refresh(c) ;::::.
           A+=.035 B+=.021 0 } ;=============================:::::::::::::::::::.
             fixture.ready("donut") ;====================::::::::::::::::::::..
                exit(fixture.canvas_loop(fn(c,zb,w,h){D(c,w,h)},0,33,0)) ;:.
                   ;.===========================:::::::::::::::::::::::..
                       ;.==================::::::::::::::::::::::::..
                           ;..=======:::::::::::::::::::::::::...
                                  ;....::::::::::::::.....
