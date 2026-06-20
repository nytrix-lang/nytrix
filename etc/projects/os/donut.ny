#!/usr/bin/env ny

;; Keywords: cli terminal donut example

                                  use ;{//////
                            ///#############///////
                        };std.core.term,std.math ;{///
                     };S="░▒▓█" mut any A=0.04 mut any
                   B=0.02 fn D(c,z,w,h){ canvas_clear(c)
                for n in 0..z.len-1{ z[n]=0.0 } a,b=sin(A)
              ,cos(A) e,f=sin(B) ,cos(B) P,Q=b-f*a ,a+f*b
             mut any v=0.0 while          v<6.28{ s,r=sin(v)
           ,cos(v) R=r+2.0 ;{               };J,N,G=e*r ;{##
          };,r*P,s*Q mut ;{                 };any u=0.0 ;{##
         };while u<6.28{                   p,q=sin(u) ;{/##*
        };,cos(u) ;{=;~                   };rs,rc=R*p ,R*q
        y0=b*rs-a*s ;{                   };z0=a*rs+b*s d=1.0
       / ;{/////#*!;~                  };(7-e*rc+f*z0) ;{**!
       #////////#*!;                 };vx=f*rc+e*z0 ;{###*!;
       };x=int(w/2 +              18.0*d*vx) if ;{#####**!=
       ##////////##*!         };x>0&&x<int(w){ y=int(h/2 -
       9.0*d*y0) if y>0&&y<int(h){ o=x+y*int(w) if ;{*!!=
       };d>z[o]{ l=J*q+N*p-G if l>0.0{ k=min(3, ;{**!!=;
       };int(l*5)) fg=k>1?15:8 bd=k>1?1:0 ch=S[k] ;{==:
        };z[o]=d canvas_set(c, x, y, ch, fg, bd) }}}}
         u+=0.05 } v+=0.14 } canvas_refresh(c) ;{;:
           };A+=0.055 B+=0.029 } ;{******!!!===;:
             };tui_canvas_loop(D) ;{!!!!===;;:
               :;===!!!!!!!!!!!!!=====;;;:-
                    :;;;;;;;;;;;;;;:};

