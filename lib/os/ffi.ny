;; Keywords: os ffi
;; Os Ffi module.

module std.os.ffi (
   RTLD_LAZY, RTLD_NOW, RTLD_GLOBAL, RTLD_LOCAL, dlopen, dlopen_any, dlsym, dlclose, dlerror,
   call0_void, call1_void, call2_void, call3_void, call4_f32_void, call0, call1, call1_i64, call2, call3, call4, call5,
   call6, call7, call8, call9, call10, call11, call12, call13, ffi_call,
   bind, call_ext, bind_all, bind_linked, import_all, import_linked, extern_all,

   ;; New C-FFI Helpers
   CStruct, CType,
   u8, i8, u16, i16, u32, i32, u64, i64, f32, f64, ptr, handle,
   sizeof_struct, offsetof_struct, malloc, free,
   set, get, bind_lib
)

use std.core as core
use std.core.dict as _d
use std.text *
use std.os *
use std.os.path as ospath

;; --- Dynamic Loading ---

fn RTLD_LAZY()   { 1 }
fn RTLD_NOW()    { 2 }
fn RTLD_GLOBAL() { 256 }
fn RTLD_LOCAL()  { 0 }

fn dlopen(path, flags) { __dlopen(path, flags) }
fn dlsym(h, s)         { __dlsym(h, s) }
fn dlclose(h)          { __dlclose(h) }
fn dlerror()           { __dlerror() }

fn _try(path, flags){
   def h = __dlopen(path, flags)
   if(h != 0){ return h }
   0
}

fn dlopen_any(name, flags=0){
   if(!core.is_str(name) || str_len(name) == 0){ return 0 }
   def n = str_len(name)
   if(n >= 4){
      if(endswith(name, ".so") || endswith(name, ".dylib") || endswith(name, ".dll")){ return _try(name, flags) }
   }
   def has_sep = ospath.has_sep(name)
   def osn = __os_name()
   if(osn == "windows"){
      def h0 = _try(name, flags)
      if(h0){ return h0 }
      def h1 = _try(name + ".dll", flags)
      if(h1){ return h1 }
      if(!has_sep){
         def h2 = _try("lib" + name + ".dll", flags)
         if(h2){ return h2 }
      }
      return 0
   }
   if(osn == "macos"){
      def h0 = _try(name, flags)
      if(h0){ return h0 }
      def h1 = _try(name + ".dylib", flags)
      if(h1){ return h1 }
      if(!has_sep){
         def h2 = _try("lib" + name + ".dylib", flags)
         if(h2){ return h2 }
      }
      return 0
   }
   ;; linux/other
   def h0 = _try(name, flags)
   if(h0){ return h0 }
   def h1 = _try(name + ".so", flags)
   if(h1){ return h1 }
   def h2 = _try(name + ".so.1", flags)
   if(h2){ return h2 }
   if(!has_sep){
      def h3 = _try("lib" + name + ".so", flags)
      if(h3){ return h3 }
      def h4 = _try("lib" + name + ".so.1", flags)
      if(h4){ return h4 }
   }
   0
}

;; --- Low-level Calls ---

fn call0_void(f)        { __call0(f) }
fn call1_void(f,a)      { __call1(f,a) }
fn call2_void(f,a,b)    { __call2(f,a,b) }
fn call3_void(f,a,b,c)  { __call3(f,a,b,c) }
fn call0(f)             { __call0(f) }
fn call1(f,a)           { __call1(f,a) }
fn call1_i64(f,a)       { __call1_i64(f,a) }
fn call2(f,a,b)         { __call2(f,a,b) }
fn call3(f,a,b,c)       { __call3(f,a,b,c) }
fn call4(f,a,b,c,d)     { __call4(f,a,b,c,d) }
fn call5(f,a,b,c,d,e)   { __call5(f,a,b,c,d,e) }
fn call6(f,a,b,c,d,e,g) { __call6(f,a,b,c,d,e,g) }
fn call7(f,a,b,c,d,e,g,h) { __call7(f,a,b,c,d,e,g,h) }
fn call8(f,a,b,c,d,e,g,h,i) { __call8(f,a,b,c,d,e,g,h,i) }
fn call9(f,a,b,c,d,e,g,h,i,j) { __call9(f,a,b,c,d,e,g,h,i,j) }
fn call10(f,a,b,c,d,e,g,h,i,j,k) { __call10(f,a,b,c,d,e,g,h,i,j,k) }
fn call11(f,a,b,c,d,e,g,h,i,j,k,l) { __call11(f,a,b,c,d,e,g,h,i,j,k,l) }
fn call12(f,a,b,c,d,e,g,h,i,j,k,l,m) { __call12(f,a,b,c,d,e,g,h,i,j,k,l,m) }
fn call13(f,a,b,c,d,e,g,h,i,j,k,l,m,n) { __call13(f,a,b,c,d,e,g,h,i,j,k,l,m,n) }
fn call4_f32_void(f,a,b,c,d) { __call4_f32_void(f,a,b,c,d) }

fn ffi_call(fptr, args){
   def n = core.len(args)
   if(n==0){ return call0(fptr)  }
   if(n==1){ return call1(fptr, core.get(args,0))  }
   if(n==2){ return call2(fptr, core.get(args,0), core.get(args,1))  }
   if(n==3){ return call3(fptr, core.get(args,0), core.get(args,1), core.get(args,2))  }
   if(n==4){ return call4(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3))  }
   if(n==5){ return call5(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3), core.get(args,4))  }
   if(n==6){ return call6(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3), core.get(args,4), core.get(args,5))  }
   if(n==7){ return call7(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3), core.get(args,4), core.get(args,5), core.get(args,6))  }
   if(n==8){ return call8(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3), core.get(args,4), core.get(args,5), core.get(args,6), core.get(args,7))  }
   if(n==9){ return call9(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3), core.get(args,4), core.get(args,5), core.get(args,6), core.get(args,7), core.get(args,8))  }
   if(n==10){ return call10(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3), core.get(args,4), core.get(args,5), core.get(args,6), core.get(args,7), core.get(args,8), core.get(args,9))  }
   if(n==11){ return call11(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3), core.get(args,4), core.get(args,5), core.get(args,6), core.get(args,7), core.get(args,8), core.get(args,9), core.get(args,10))  }
   if(n==12){ return call12(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3), core.get(args,4), core.get(args,5), core.get(args,6), core.get(args,7), core.get(args,8), core.get(args,9), core.get(args,10), core.get(args,11))  }
   if(n==13){ return call13(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3), core.get(args,4), core.get(args,5), core.get(args,6), core.get(args,7), core.get(args,8), core.get(args,9), core.get(args,10), core.get(args,11), core.get(args,12))  }
   panic("ffi_call supports 0-13 args")
}

;; --- Binding ---

fn bind(h, name){
   def fptr = dlsym(h, name)
   if(fptr != 0){
      return fn(...args){ ffi_call(fptr, args) }
   }
   0
}

fn call_ext(h, name, ...args){
   def fptr = dlsym(h, name)
   if(fptr != 0){ return ffi_call(fptr, args) }
   0
}

fn bind_all(h, names){
   mut res = _d.dict()
   mut i = 0 mut n = core.len(names)
   while(i < n){
      def name = core.get(names, i)
      def b = bind(h, name)
      if(b != 0){ res = _d.dict_set(res, name, b) }
      i += 1
   }
   res
}

fn bind_linked(names){ bind_all(0, names) }

fn import_all(h, names){
   mut g = __globals()
   mut i = 0 mut n = core.len(names)
   while(i < n){
      def name = core.get(names, i)
      def b = bind(h, name)
      if(b != 0){ _d.dict_set(g, name, b) }
      i += 1
   }
   true
}

fn import_linked(names){ import_all(0, names) }
fn extern_all(){ 0 }

;; --- Memory ---

fn malloc(n) { if(n<=0){0} __malloc(n) }
fn free(p)   { if(p){ __free(p) } 0 }

;; --- C-Type definitions ---

fn u8()   { ["u8", 1, 1] }
fn i8()   { ["i8", 1, 1] }
fn u16()  { ["u16", 2, 2] }
fn i16()  { ["i16", 2, 2] }
fn u32()  { ["u32", 4, 4] }
fn i32()  { ["i32", 4, 4] }
fn u64()  { ["u64", 8, 8] }
fn i64()  { ["i64", 8, 8] }
fn f32()  { ["f32", 4, 4] }
fn f64()  { ["f64", 8, 8] }
fn ptr()  { ["ptr", 8, 8] }
fn handle() { ["ptr", 8, 8] }

fn _align(offset, alignment){
   def rem = offset % alignment
   if(rem == 0){ return offset }
   offset + (alignment - rem)
}

fn CStruct(fields){
   mut d_lyt = core.dict()
   mut curr_off = 0
   mut m_align = 1
   mut i = 0
   while(i < core.len(fields)){
      def fld = core.get(fields, i)
      def typ = core.get(fld, 0)
      def nm = core.get(fld, 1)
      def sz = core.get(typ, 1)
      def al = core.get(typ, 2)
      curr_off = _align(curr_off, al)
      mut info = core.dict()
      info = _d.dict_set(info, "offset", curr_off)
      info = _d.dict_set(info, "type", core.get(typ, 0))
      info = _d.dict_set(info, "size", sz)
      d_lyt = _d.dict_set(d_lyt, nm, info)
      if(al > m_align){ m_align = al }
      curr_off += sz
      i += 1
   }
   def t_size = _align(curr_off, m_align)
   mut sd = core.dict()
   sd = _d.dict_set(sd, "fields", d_lyt)
   sd = _d.dict_set(sd, "size", t_size)
   sd = _d.dict_set(sd, "align", m_align)
   sd
}

fn sizeof_struct(d){
   if(core.is_list(d)){ return core.get(d, 1) }
   _d.dict_get(d, "size", 0)
}

fn offsetof_struct(d, name){
   def fs = _d.dict_get(d, "fields", 0)
   def info = _d.dict_get(fs, name, 0)
   _d.dict_get(info, "offset", -1)
}

fn set(p, d, name, val){
   def fs = _d.dict_get(d, "fields", 0)
   def info = _d.dict_get(fs, name, 0)
   if(!info){ return false }
   def off = _d.dict_get(info, "offset")
   def typ = _d.dict_get(info, "type")
   if(eq(typ, "u32") || eq(typ, "i32")){ core.store32(p, val, off) }
   elif(eq(typ, "u64") || eq(typ, "i64") || eq(typ, "ptr")){ core.store64_raw(p, val, off) }
   elif(eq(typ, "f32")){ core.store32_f32(p, val, off) }
   elif(eq(typ, "u8") || eq(typ, "i8")){ core.store8(p, val, off) }
   elif(eq(typ, "u16") || eq(typ, "i16")){ core.store16(p, val, off) }
   true
}

fn get(p, d, name){
   def fs = _d.dict_get(d, "fields", 0)
   def info = _d.dict_get(fs, name, 0)
   if(!info){ return 0 }
   def off = _d.dict_get(info, "offset")
   def typ = _d.dict_get(info, "type")
   if(eq(typ, "u32") || eq(typ, "i32")){ return core.load32(p, off) }
   elif(eq(typ, "u64") || eq(typ, "i64") || eq(typ, "ptr")){ return core.load64(p, off) }
   elif(eq(typ, "f32")){ return core.load32_f32(p, off) }
   elif(eq(typ, "u8") || eq(typ, "i8")){ return core.load8(p, off) }
   elif(eq(typ, "u16") || eq(typ, "i16")){ return core.load16(p, off) }
   0
}

fn bind_lib(path, syms){
   def lib = dlopen_any(path, RTLD_NOW())
   if(!lib){ return 0 }
   mut res = _d.dict()
   mut i = 0
   while(i < core.len(syms)){
      def s = core.get(syms, i)
      mut nm = "" mut arity = -1
      if(core.is_list(s)){ nm = core.get(s, 0) arity = core.get(s, 1) }
      else { nm = s }
      def fptr = dlsym(lib, nm)
      if(fptr){
         if(arity >= 0){
            res = _d.dict_set(res, nm, fn(...args){
               if(core.len(args) != arity){ panic(cat("Arity mismatch for ", nm)) }
               ffi_call(fptr, args)
            })
         } else {
            res = _d.dict_set(res, nm, fn(...args){ ffi_call(fptr, args) })
         }
      }
      i += 1
   }
   res
}
