#!/usr/bin/env ny
;; Keywords: cli terminal donut generator lab
;; donutgen - emits the compact terminal donut (etc/labs/donut.ny).
;;
;; usage: donutgen.ny [--out PATH] [--preview] [--dry-run]
use core
use core.os (file_write)

def HEADER = "#!/usr/bin/env ny\n" +
   ";; Keywords: cli terminal donut\n" +
   ";; Donut - https://www.a1k0n.net/2006/09/15/obfuscated-c-donut.html\n"

def PROG =    "\n" +
   "              use core;=========--\n" +
   "         use core.term mut A,B=.55;----\n" +
   "      ,.35 fn R(u,v,a,b,c,d,L,M,N){g=4.41-\n" +
   "    u*u-v*v if g<=0{return}mut t=sqrt(g)+.01\n" +
   "  for _ in 0..56{p=c*u+d*t x=d*u-c*t y=b*v+a*;\n" +
   " p z=b*p-a*v r=sqrt(x*x+y*y)q=r-1.48 e=sqrt(q*;\n" +
   "q+z*z)if e<.632{l=q*(x*L+y*M)+z*N*r return l<.2*\n" +
   "e*r?1:l<.65*e*r?2:            3}t-=e-.62}0}fn D(\n" +
   "any c,z,w,h)int                  {def i,j=int(;.\n" +
   "w),int(h)a,b,                    d,e=sin(A),cos(\n" +
   "A),sin(B),cos(B)z=            .65*e-.45*d L,M,N=\n" +
   "-.45*e-.65*d,.65*b+z*a,z*b-.65*a mut f64 s=min(;\n" +
   " i+1,j*2+1)s*=.18 v=(j-1)/s for y in 0..j{u=;..\n" +
   "  (1-i)*.5/s for x in 0..i{k=R(u,v,a,b,d,e,L,M\n" +
   "    ,N)canvas_set(c,x,y,\" ░▒▓\"[k],[0,4,5,7][\n" +
   "      k],k/3)u+=1/s}v-=2/s}canvas_refresh(\n" +
   "         c)A+=.035 B+=.021;-...........\n" +
   "              0}tui_canvas_loop(D)\n"

def OUT_DEFAULT = "etc/labs/donut.ny"

fn usage() {
   print("usage: donutgen.ny [--out PATH] [--preview] [--dry-run]")
   print("  --out PATH   write the generated program there (default " + OUT_DEFAULT + ")")
   print("  --preview    print the generated program")
   print("  --dry-run    report without writing")
}

fn newlines(str s) int {
   mut n = 0
   mut i = 0
   while i < s.len {
      if load8(s, i) == 10 { n += 1 }
      i += 1
   }
   n
}

fn main() int {
   mut out_path = OUT_DEFAULT
   mut dry = false
   mut preview = false
   mut i = 1
   while i < argc() {
      def a = argv(i)
      if a == "--out" && i + 1 < argc() {
         i += 1
         out_path = argv(i)
      } elif a == "--preview" {
         preview = true
      } elif a == "--dry-run" {
         dry = true
      } elif a == "--help" || a == "-h" {
         usage()
         return 0
      } else {
         print("donutgen: unknown argument " + a)
         return 2
      }
      i += 1
   }
   def text = HEADER + PROG
   if preview { print(text) }
   if dry {
      print("would write " + out_path + " (" + to_str(text.len) + " bytes, " + to_str(newlines(text)) + " lines)")
      return 0
   }
   file_write(out_path, text)
   print("wrote " + out_path + " (" + to_str(text.len) + " bytes, " + to_str(newlines(text)) + " lines)")
   0
}

exit(main())
