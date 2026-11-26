#!/usr/bin/env ny

;; Keywords: cli terminal image graphics braille unicode ansi
;; Img - native terminal image renderer
;; Modes: braille (2x4), block (2x2), half (1x2), ascii.
;; Colors: TrueColor, ANSI 256, monochrome.
;; Usage: ny img.ny [options] IMAGE
;;        ny img.ny --help

use core
use core.os (argv, env, exit)
use core.os.sys (sys_ioctl)
use core.math.parse.img as image
use core.str as txt

def dots = [1,8,2,16,4,32,64,128]
def blocks = [" ","▘","▝","▀","▖","▌","▞","▛","▗","▚","▐","▜","▄","▙","▟","█"]
def halves = [" ","▀","▄","█"]
def ramp = " .:-=+*#%@"
def bayer = [0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5]
def esc = txt.chr(27) + "["

fn lo(int a,int b) int { a < b ? a : b }
fn hi(int a,int b) int { a > b ? a : b }
fn clip(int x,int a,int b) int { hi(a,lo(x,b)) }
fn red(int p) int { p & 255 }
fn green(int p) int { (p >> 8) & 255 }
fn blue(int p) int { (p >> 16) & 255 }
fn rgb(int r,int g,int b) int { r | (g << 8) | (b << 16) }
fn lum(int p) int { (54*red(p)+183*green(p)+19*blue(p)+128)/256 }
fn sq(int x) int { x*x }
fn dist(int a,int b) int { 2*sq(red(a)-red(b))+4*sq(green(a)-green(b))+sq(blue(a)-blue(b)) }
fn mean(int r,int g,int b,int n) int { rgb((r+n/2)/n,(g+n/2)/n,(b+n/2)/n) }
fn gain(int r,int g,int b,int n,int p) int {
   2*(2*r*red(p)-n*sq(red(p)))+4*(2*g*green(p)-n*sq(green(p)))+2*b*blue(p)-n*sq(blue(p))
}
fn pop(int m) int {
   mut n=0
   while m != 0 { m=m & (m-1) n+=1 }
   n
}
fn glyph(str mode,int mask) str {
   if mode == "block" { return str(blocks.get(mask)) }
   if mode == "half" { return str(halves.get(mask)) }
   mut code=0
   for i in range(8) { if (mask & (1 << i)) != 0 { code=code | int(dots.get(i)) } }
   txt.chr(0x2800+code)
}
fn envnum(str name,int fallback) int {
   def v=env(name)
   if !is_str(v) { return fallback }
   def n=txt.atoi(v)
   n > 0 ? n : fallback
}
fn terminal() list {
   def p=zalloc(8)
   defer { free(p) }
   mut tty=false
   mut w=envnum("COLUMNS",80)
   mut h=envnum("LINES",24)
   #linux {
      if is_ok(sys_ioctl(1,0x5413,p)) {
         tty=true
         def rows=load8(p,0) | (load8(p,1) << 8)
         def cols=load8(p,2) | (load8(p,3) << 8)
         if cols > 0 { w=cols }
         if rows > 0 { h=rows }
      }
   }
   [w,hi(1,h-1),tty]
}
fn sample(any data,int iw,int ih,int ch,int x,int y,int dw,int dh,int bg) int {
   def x0=x*iw
   def x1=(x+1)*iw
   def y0=y*ih
   def y1=(y+1)*ih
   mut r=0
   mut g=0
   mut b=0
   mut iy=y0/dh
   while iy*dh < y1 {
      def wy=lo(y1,(iy+1)*dh)-hi(y0,iy*dh)
      mut ix=x0/dw
      while ix*dw < x1 {
         def weight=wy*(lo(x1,(ix+1)*dw)-hi(x0,ix*dw))
         def off=(iy*iw+ix)*ch
         def alpha=ch == 4 ? load8(data,off+3) : 255
         def base=(255-alpha)*bg
         r+=weight*(load8(data,off)*alpha+base)
         g+=weight*(load8(data,off+1)*alpha+base)
         b+=weight*(load8(data,off+2)*alpha+base)
         ix+=1
      }
      iy+=1
   }
   def total=iw*ih*255
   mean(r,g,b,total)
}
fn fit(any cell,int n) list {
   mut tr=0
   mut tg=0
   mut tb=0
   for i in range(n) {
      def p=load32(cell,i*4)
      tr+=red(p) tg+=green(p) tb+=blue(p)
   }
   mut bg=mean(tr,tg,tb,n)
   mut fg=bg
   mut mask=0
   mut best=gain(tr,tg,tb,n,bg)
   mut r=0
   mut g=0
   mut b=0
   mut count=0
   mut old=0
   mut i=1
   while i < (1 << (n-1)) {
      def m=i ^ (i >> 1)
      def changed=m ^ old
      mut bit=0
      while (changed & (1 << bit)) == 0 { bit+=1 }
      def p=load32(cell,bit*4)
      def sign=(m & changed) != 0 ? 1 : -1
      r+=sign*red(p) g+=sign*green(p) b+=sign*blue(p) count+=sign
      def c0=mean(r,g,b,count)
      def c1=mean(tr-r,tg-g,tb-b,n-count)
      def score=gain(r,g,b,count,c0)+gain(tr-r,tg-g,tb-b,n-count,c1)
      if score > best {
         best=score mask=m fg=c0 bg=c1
      }
      old=m
      i+=1
   }
   if pop(mask) > n/2 { return [mask ^ ((1 << n)-1),bg,fg] }
   [mask,fg,bg]
}
fn cube(int x) int { x < 48 ? 0 : (x < 115 ? 1 : (x-35)/40) }
fn level(int x) int { x == 0 ? 0 : 55+40*x }
fn palette(int p) int {
   def r=cube(red(p))
   def g=cube(green(p))
   def b=cube(blue(p))
   def gray=clip((2*red(p)+4*green(p)+blue(p)-21)/70,0,23)
   def v=8+10*gray
   dist(p,rgb(v,v,v)) < dist(p,rgb(level(r),level(g),level(b))) ? 232+gray : 16+36*r+6*g+b
}
fn ansi(int p,bool fg,str colors) str {
   def prefix=esc+(fg ? "38;" : "48;")
   if colors == "256" { return prefix+"5;"+to_str(palette(p))+"m" }
   prefix+"2;"+to_str(red(p))+";"+to_str(green(p))+";"+to_str(blue(p))+"m"
}
fn render(any im,int w,int h,str mode,str colors,int threshold,bool invert,bool dither,int bg) int {
   def iw=int(im.get("width",0))
   def ih=int(im.get("height",0))
   def ch=int(im.get("channels",4))
   def data=im.get("data",0)
   if iw < 1 || ih < 1 || iw > 65536 || ih > 65536 || (ch != 3 && ch != 4) || !is_str(data) || data.len < iw*ih*ch {
      eprint("img: invalid or unsupported pixel buffer")
      return 1
   }
   def sx=mode == "half" ? 1 : 2
   def sy=(mode == "half" || mode == "block") ? 2 : 4
   def n=sx*sy
   def dw=w*sx
   def dh=h*sy
   def cell=zalloc(32)
   defer { free(cell) }
   for y in range(h) {
      mut ptr row=txt.Builder(w*48+16)
      mut lastfg=-1
      mut lastbg=-1
      for x in range(w) {
         mut mask=0
         mut light=0
         for i in range(n) {
            def px=x*sx+i%sx
            def py=y*sy+i/sx
            def p=sample(data,iw,ih,ch,px,py,dw,dh,bg)
            store32(cell,p,i*4)
            def l=invert ? 255-lum(p) : lum(p)
            light+=l
            def t=threshold+(dither ? 8*int(bayer.get((py%4)*4+px%4))-60 : 0)
            if l >= clip(t,0,255) { mask=mask | (1 << i) }
         }
         if mode == "ascii" {
            row=txt.builder_append_byte(row,load8(ramp,light*(ramp.len-1)/(255*n)))
         } elif colors == "none" {
            row=txt.builder_append(row,glyph(mode,mask))
         } else {
            def f=fit(cell,n)
            mask=int(f.get(0))
            def fg=int(f.get(1))
            def back=int(f.get(2))
            if back != lastbg { row=txt.builder_append(row,ansi(back,false,colors)) lastbg=back }
            if mask != 0 && fg != lastfg { row=txt.builder_append(row,ansi(fg,true,colors)) lastfg=fg }
            row=txt.builder_append(row,mask == 0 ? " " : glyph(mode,mask))
         }
      }
      if colors != "none" && mode != "ascii" { row=txt.builder_append(row,esc+"0m") }
      print(txt.builder_to_str(row))
      txt.builder_free(row)
   }
   0
}
fn usage() {
   print("img [options] IMAGE\n  -m, --mode braille|block|half|ascii\n  -c, --colors auto|full|256|none\n  -w, --width COLS\n  -H, --height ROWS\n  -t, --threshold 0..255\n  -i, --invert       invert monochrome\n  -d, --dither       ordered monochrome dithering\n  --bg black|white   alpha compositing\n  --selftest\n  -h, --help")
}
fn fail(str msg) int { eprint("img: "+msg) 2 }
fn selftest() int {
   assert(glyph("braille",1) == "⠁","dot 1")
   assert(glyph("braille",2) == "⠈","dot 4")
   assert(glyph("braille",64) == "⡀","dot 7")
   assert(glyph("braille",128) == "⢀","dot 8")
   assert(glyph("braille",255) == "⣿","all dots")
   assert(glyph("block",9) == "▚","quadrant")
   def p=zalloc(32)
   defer { free(p) }
   for i in range(8) { store32(p,i == 0 ? 0xffffff : 0,i*4) }
   def f=fit(p,8)
   assert(f.get(0) == 1 && f.get(1) == 0xffffff && f.get(2) == 0,"color partition")
   for i in range(8) { store32(p,0x123456,i*4) }
   def solid=fit(p,8)
   assert(solid.get(0) == 0 && solid.get(2) == 0x123456,"solid color")
   assert(palette(0xff) == 196,"red palette")
   store32(p,0xff0000ff,0)
   store32(p,0xff000000,4)
   assert(sample(p,2,1,4,0,0,1,1,0) == 128,"area filter")
   store32(p,0,0)
   assert(sample(p,1,1,4,0,0,1,1,255) == 0xffffff,"alpha compositing")
   print("img: selftest passed")
   0
}
fn main() int {
   def term=terminal()
   mut w=int(term.get(0))
   mut h=int(term.get(1))
   mut mode="braille"
   mut colors="auto"
   mut threshold=128
   mut invert=false
   mut dither=false
   mut bg=0
   mut path=""
   mut i=1
   mut explicitw=false
   mut explicith=false
   while i < argc() {
      def a=str(argv(i))
      if a == "-h" || a == "--help" { usage() return 0 }
      if a == "--selftest" { return selftest() }
      if a == "-i" || a == "--invert" { invert=true }
      elif a == "-d" || a == "--dither" { dither=true }
      elif a == "--" {
         if i+1 >= argc() || path != "" { return fail("expected image after --") }
         i+=1 path=str(argv(i))
      } elif a == "-w" || a == "--width" || a == "-H" || a == "--height" || a == "-m" || a == "--mode" || a == "-c" || a == "--colors" || a == "-t" || a == "--threshold" || a == "--bg" {
         if i+1 >= argc() { return fail("missing value for "+a) }
         i+=1
         def v=str(argv(i))
         if a == "-w" || a == "--width" { if !txt.ascii_all_digit(v) { return fail("invalid width") } w=txt.atoi(v) explicitw=true }
         elif a == "-H" || a == "--height" { if !txt.ascii_all_digit(v) { return fail("invalid height") } h=txt.atoi(v) explicith=true }
         elif a == "-m" || a == "--mode" { mode=v }
         elif a == "-c" || a == "--colors" { colors=v }
         elif a == "-t" || a == "--threshold" { if !txt.ascii_all_digit(v) { return fail("invalid threshold") } threshold=txt.atoi(v) }
         elif v == "white" { bg=255 }
         elif v == "black" { bg=0 }
         else { return fail("background must be black or white") }
      } elif txt.startswith(a,"-") { return fail("unknown option "+a) }
      elif path == "" { path=a }
      else { return fail("only one image may be supplied") }
      i+=1
   }
   if path == "" { usage() return 2 }
   if mode != "braille" && mode != "block" && mode != "half" && mode != "ascii" { return fail("unknown mode "+mode) }
   if colors == "auto" { colors=term.get(2) ? "full" : "none" }
   if colors != "full" && colors != "256" && colors != "none" { return fail("unknown color mode "+colors) }
   if mode == "ascii" { colors="none" }
   if w < 1 || w > 4096 || h < 1 || h > 4096 || threshold < 0 || threshold > 255 { return fail("invalid dimensions or threshold") }
   def im=image.load(path)
   if !im { eprint("img: cannot decode "+path) return 1 }
   def iw=int(im.get("width",0))
   def ih=int(im.get("height",0))
   if iw < 1 || ih < 1 || iw > 65536 || ih > 65536 { return fail("invalid image dimensions") }
   if explicitw && !explicith { h=hi(1,(w*ih+iw)/(2*iw)) }
   elif explicith && !explicitw { w=hi(1,(2*h*iw+ih/2)/ih) }
   elif w*ih > 2*h*iw { w=hi(1,(2*h*iw+ih/2)/ih) }
   else { h=hi(1,(w*ih+iw)/(2*iw)) }
   if w > 4096 || h > 4096 || w*h > 1000000 { return fail("output too large; specify smaller dimensions") }
   render(im,w,h,mode,colors,threshold,invert,dither,bg)
}
exit(main())
