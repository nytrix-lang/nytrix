#!/usr/bin/env ny
;; Keywords: cli terminal donut
;; Donut - https://www.a1k0n.net/2006/09/15/obfuscated-c-donut.html

                                            use core use ;{////
                                     };core.os (exit) use core.term
                                use core.os.fixture as fixture mut ;{///
                             };z=own(bytes(1)) mut p=own(bytes(1)) mut ;{/
                         };cap=0 mut A=.04 mut B=.02 fn resize(int w,int ;{//
                      };h)int{if w<1||h<1{return 0} def n=w*h if n!=cap{cap=n
                    z=own(bytes(n*2)) p=own(bytes(n))}0} fn draw(any c,int w,int
                 h,float a,float b)int{ mut s=w*.75 if (h-1)*1.5<s{s=(h-1)*1.5}
               def X=s def Y=s*.5 mut i=0 while i<w*h{bytes_set(p,i,0) ;{////////#
             };bytes_set(z,i*2,0) bytes_set(z,i*2+1,0) i+=1} def sa=sin(a) def ;{/#
            };ca=cos(a) def sb=sin(b) def cb=cos(b) def P=ca-cb*sa def Q=sa+cb*ca ;{
          };mut v=0.0 while v<6.28{def ;{                };sv=sin(v) def cv=cos(v)
        def R=cv*.75+2.8 def J=sb*cv                       def N=cv*P def G=sv*Q mut
       u=0.0 while u<6.28{def ;{-                           };su=sin(u) def cu=cos(u)
      def rs=R*su def rc=R*cu                               def yy=ca*rs-sa*sv def ;{
     };zz=sa*rs+ca*sv def ;{-                              };d=1/(7-sb*rc+cb*zz) def
    ;{/////////###**!!=;;:-                               !*###//////////////////###*
   #//////////###**!!=;;:,                               !*###//////////////////###**
  #///////////###**!==;~                                **###/////////////////#####**
 *////////////##**!!=;:                               };x=int(w/2+X*d*(cb*rc+sb*zz))
 def y=int(h/2-Y*d*yy)                               if x>=0&&x<w&&y>0&&y<h{def ;{**!
};o=x+y*w def k=o*2 ;{                             };def dn=int(d*65535) if ;{###**!
##////////////###*!=:                            *####///////////////////######***!=
##////////////###*!=;                          ####///////////////////########***!!
##/////////////###*!=                       #####////////////////////#######****!!
*#//////////////###*!;                  *#####////////////////////#########***!!!;
*##///////////////###*!             ######/////////////////////##########****!!==
};dn>bytes_get(z,k)*256+bytes_get(z,k+1){bytes_set(z,k,dn/256) ;{######*****!!=;
};bytes_set(z,k+1,dn%256) def l=J*cu+N*su-G mut sh=int((l+1)*5.5)+1 if ;{*!!!=
 };sh<1{sh=1} if sh>12{sh=12} bytes_set(p,o,sh)}} u+=.025} v+=.02} mut y=1 ;{
 };while y<h{mut x=0 while x<w{def k=bytes_get(p,x+y*w) def ;{##******!!!==;
  };col=k==0?0:k<4?4:k<7?5:k<10?6:7 canvas_set(c,x,y,k>0? "█" : " " ;{!==;
   };,col,k>0&&k%3==0?1:0) x+=1} y+=1} canvas_print(c,0,0, ;{***!!!!===;
    };"q/esc · Donut" ,6,1) canvas_refresh(c) 0 } fixture.ready( ;{=;;
      };"donut" ) exit(fixture.canvas_loop(fn(c,zb,w,h){resize(w,h)
       draw(c,w,h,A,B) A+=.055 B+=.029 0},0,33,0)) ;{!!!!!!!===;;:
         ;==!!!*********************************!!!!!!!!====;;:
            ;==!!!!!!!******************!!!!!!!!!!======;;::
               ;;===!!!!!!!!!!!!!!!!!!!!!!!========;;;::
                   :;;;=====================;;;;;::
                         :::;;;;;;;;;;;:::};
