;; Keywords: os ffi
;; Os Ffi module.

use std.core as core
use std.core.dict as _d
module std.os.ffi (
   RTLD_LAZY, RTLD_NOW, RTLD_GLOBAL, RTLD_LOCAL, dlopen, dlsym, dlclose, dlerror,
   call0_void, call1_void, call2_void, call3_void, call0, call1, call2, call3, call4, call5,
   call6, call7, call8, call9, call10, call11, call12, call13, ffi_call,
   bind, call_ext, bind_all, bind_linked, import_all, import_linked, extern_all
)

fn RTLD_LAZY(){ "dlopen flag: resolve symbols lazily." return 1 }
fn RTLD_NOW(){ "dlopen flag: resolve symbols immediately." return 2 }
fn RTLD_GLOBAL(){ "dlopen flag: make symbols globally available." return 256 }
fn RTLD_LOCAL(){ "dlopen flag: keep symbols local." return 0 }

fn dlopen(path, flags){ "Opens a dynamic library." return __dlopen(path, flags) }
fn dlsym(handle, symbol){ "Resolves a symbol." return __dlsym(handle, symbol) }
fn dlclose(handle){ "Closes a library handle." return __dlclose(handle) }
fn dlerror(){ "Returns the last error." return __dlerror() }

fn call0_void(fptr){ "Calls `fptr()` and ignores the return value." __call0(fptr) }
fn call1_void(fptr,a){ "Calls `fptr(a)` and ignores the return value." __call1(fptr,a) }
fn call2_void(fptr,a,b){ "Calls `fptr(a,b)` and ignores the return value." __call2(fptr,a,b) }
fn call3_void(fptr,a,b,c){ "Calls `fptr(a,b,c)` and ignores the return value." __call3(fptr,a,b,c) }

fn call0(fptr){ "Calls `fptr()` and returns the raw result." return __call0(fptr) }
fn call1(fptr,a){ "Calls `fptr(a)` and returns the raw result." return __call1(fptr,a) }
fn call2(fptr,a,b){ "Calls `fptr(a,b)` and returns the raw result." return __call2(fptr,a,b) }
fn call3(fptr,a,b,c){ "Calls `fptr(a,b,c)` and returns the raw result." return __call3(fptr,a,b,c) }
fn call4(fptr,a,b,c,d){ "Calls `fptr(a,b,c,d)` and returns the raw result." return __call4(fptr,a,b,c,d) }
fn call5(fptr,a,b,c,d,e){ "Calls `fptr(a,b,c,d,e)` and returns the raw result." return __call5(fptr,a,b,c,d,e) }
fn call6(fptr,a,b,c,d,e,g){ "Calls `fptr(a,b,c,d,e,g)` and returns the raw result." return __call6(fptr,a,b,c,d,e,g) }
fn call7(fptr,a,b,c,d,e,g,h){ "Calls `fptr(a,b,c,d,e,g,h)` and returns the raw result." return __call7(fptr,a,b,c,d,e,g,h) }
fn call8(fptr,a,b,c,d,e,g,h,i){ "Calls `fptr(a,b,c,d,e,g,h,i)` and returns the raw result." return __call8(fptr,a,b,c,d,e,g,h,i) }
fn call9(fptr,a,b,c,d,e,g,h,i,j){ "Calls `fptr(a,b,c,d,e,g,h,i,j)` and returns the raw result." return __call9(fptr,a,b,c,d,e,g,h,i,j) }
fn call10(fptr,a,b,c,d,e,g,h,i,j,k){ "Calls `fptr(a,b,c,d,e,g,h,i,j,k)` and returns the raw result." return __call10(fptr,a,b,c,d,e,g,h,i,j,k) }
fn call11(fptr,a,b,c,d,e,g,h,i,j,k,l){ "Calls `fptr(a,b,c,d,e,g,h,i,j,k,l)` and returns the raw result." return __call11(fptr,a,b,c,d,e,g,h,i,j,k,l) }
fn call12(fptr,a,b,c,d,e,g,h,i,j,k,l,m){ "Calls `fptr(a,b,c,d,e,g,h,i,j,k,l,m)` and returns the raw result." return __call12(fptr,a,b,c,d,e,g,h,i,j,k,l,m) }
fn call13(fptr,a,b,c,d,e,g,h,i,j,k,l,m,n){ "Calls `fptr(a,b,c,d,e,g,h,i,j,k,l,m,n)` and returns the raw result." return __call13(fptr,a,b,c,d,e,g,h,i,j,k,l,m,n) }

fn ffi_call(fptr, args){
   "Calls `fptr` with arguments from `args` (supported arity: 0..13)."
   def n = core.list_len(args)
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

fn bind(handle, name){
   "Resolves `name` from `handle` and returns a callable wrapper, or 0."
   def fptr = dlsym(handle, name)
   if(fptr != 0){
      return fn(...args){ return ffi_call(fptr, args) }
   }
   return 0
}

fn call_ext(handle, name, ...args){
   "Resolves and invokes `name` from `handle` with variadic `args`."
   def fptr = dlsym(handle, name)
   if(fptr != 0){ return ffi_call(fptr, args) }
   return 0
}

fn bind_all(handle, names){
   "Resolves each symbol in `names` and returns a dict of bound callables."
   mut res = _d.dict()
   mut i = 0 mut n = core.list_len(names)
   while(i < n){
      def name = core.get(names, i)
      def b = bind(handle, name)
      if(b != 0){ res = core.set_idx(res, name, b) }
      i += 1
   }
   res
}

fn bind_linked(names){
   "Binds all `names` from the process default (linked) symbols and returns a dict."
   bind_all(0, names)
}

fn import_all(handle, names){
   "Imports all resolvable symbols into the global table."
   mut g = __globals()
   if(!core.is_dict(g)){
      g = _d.dict(core.list_len(names) + 8)
      __set_globals(g)
   }
   mut i = 0 mut n = core.list_len(names)
   while(i < n){
      def name = core.get(names, i)
      def b = bind(handle, name)
      if(b != 0){
         def new_g = core.set_idx(g, name, b)
         __set_globals(new_g)
         g = new_g
      }
      i += 1
   }
   return true
}

fn import_linked(names){
   "Imports all `names` from the process default (linked) symbols into globals."
   import_all(0, names)
}

fn extern_all(names){
   "Registers extern functions by name (or [name, arity]) for linked symbols."
   0
}
