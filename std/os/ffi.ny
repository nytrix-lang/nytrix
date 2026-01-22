;; Keywords: os ffi
;; Os Ffi module.

use std.core
module std.os.ffi (
   RTLD_LAZY, RTLD_NOW, RTLD_GLOBAL, RTLD_LOCAL, dlopen, dlsym, dlclose, dlerror,
   call0_void, call1_void, call2_void, call3_void, call0, call1, call2, call3, call4, call5,
   call6, call7, call8, call9, call10, call11, call12, call13, ffi_call
)

fn RTLD_LAZY(){
   "dlopen flag: resolve symbols lazily."
   return 1
}

fn RTLD_NOW(){
   "dlopen flag: resolve symbols immediately."
   return 2
}

fn RTLD_GLOBAL(){
   "dlopen flag: make symbols globally available."
   return 256
}

fn RTLD_LOCAL(){
   "dlopen flag: keep symbols local."
   return 0
}

fn dlopen(path, flags){
   "Open shared object and return handle or 0."
   return __dlopen(path, flags)
}

fn dlsym(handle, name){
   "Lookup symbol and return function/data pointer as int."
   return __dlsym(handle, name)
}

fn dlclose(handle){
   "Close shared object handle."
   return __dlclose(handle)
}

fn dlerror(){
   "Return last dl error string pointer."
   return __dlerror()
}

fn call0_void(fptr){
   "Call fptr with 0 args and return 0."
   __call0(fptr)
   return 0
}

fn call1_void(fptr,a){
   "Call fptr with 1 arg and return 0."
   __call1(fptr,a)
   return 0
}

fn call2_void(fptr,a,b){
   "Call fptr with 2 args and return 0."
   __call2(fptr,a,b)
   return 0
}

fn call3_void(fptr,a,b,c){
   "Call fptr with 3 args and return 0."
   __call3(fptr,a,b,c)
   return 0
}

fn call0(fptr){
   "Call fnptr with 0-3 int64 args."
   return __call0(fptr)
}

fn call1(fptr,a){
   "Call fptr with 1 int64 arg."
   return __call1(fptr,a)
}

fn call2(fptr,a,b){
   "Call fptr with 2 int64 args."
   return __call2(fptr,a,b)
}

fn call3(fptr,a,b,c){
   "Call fptr with 3 int64 args."
   return __call3(fptr,a,b,c)
}

fn call4(fptr,a,b,c,d){
   "Call fptr with 4 int64 args."
   return __call4(fptr,a,b,c,d)
}

fn call5(fptr,a,b,c,d,e){
   "Call fptr with 5 int64 args."
   return __call5(fptr,a,b,c,d,e)
}

fn call6(fptr,a,b,c,d,e,g){
   "Call fptr with 6 int64 args."
   return __call6(fptr,a,b,c,d,e,g)
}

fn call7(fptr,a,b,c,d,e,g,h){
   "Call fptr with 7 int64 args."
   return __call7(fptr,a,b,c,d,e,g,h)
}

fn call8(fptr,a,b,c,d,e,g,h,i){
   "Call fptr with 8 int64 args."
   return __call8(fptr,a,b,c,d,e,g,h,i)
}

fn call9(fptr,a,b,c,d,e,g,h,i,j){
   "Call fptr with 9 int64 args."
   return __call9(fptr,a,b,c,d,e,g,h,i,j)
}

fn call10(fptr,a,b,c,d,e,g,h,i,j,k){
   "Call fptr with 10 int64 args."
   return __call10(fptr,a,b,c,d,e,g,h,i,j,k)
}

fn call11(fptr,a,b,c,d,e,g,h,i,j,k,l){
   "Call fptr with 11 int64 args."
   return __call11(fptr,a,b,c,d,e,g,h,i,j,k,l)
}

fn call12(fptr,a,b,c,d,e,g,h,i,j,k,l,m){
   "Call fptr with 12 int64 args."
   return __call12(fptr,a,b,c,d,e,g,h,i,j,k,l,m)
}

fn call13(fptr,a,b,c,d,e,g,h,i,j,k,l,m,n){
   "Call fptr with 13 int64 args."
   return __call13(fptr,a,b,c,d,e,g,h,i,j,k,l,m,n)
}

fn ffi_call(fptr, args){
   "Call with list args (0-12 supported)."
   def n = list_len(args)
   if(n==0){ return call0(fptr)  }
   if(n==1){ return call1(fptr, get(args,0))  }
   if(n==2){ return call2(fptr, get(args,0), get(args,1))  }
   if(n==3){ return call3(fptr, get(args,0), get(args,1), get(args,2))  }
   if(n==4){ return call4(fptr, get(args,0), get(args,1), get(args,2), get(args,3))  }
   if(n==5){ return call5(fptr, get(args,0), get(args,1), get(args,2), get(args,3), get(args,4))  }
   if(n==6){ return call6(fptr, get(args,0), get(args,1), get(args,2), get(args,3), get(args,4), get(args,5))  }
   if(n==7){ return call7(fptr, get(args,0), get(args,1), get(args,2), get(args,3), get(args,4), get(args,5), get(args,6))  }
   if(n==8){ return call8(fptr, get(args,0), get(args,1), get(args,2), get(args,3), get(args,4), get(args,5), get(args,6), get(args,7))  }
   if(n==9){ return call9(fptr, get(args,0), get(args,1), get(args,2), get(args,3), get(args,4), get(args,5), get(args,6), get(args,7), get(args,8))  }
   if(n==10){ return call10(fptr, get(args,0), get(args,1), get(args,2), get(args,3), get(args,4), get(args,5), get(args,6), get(args,7), get(args,8), get(args,9))  }
   if(n==11){ return call11(fptr, get(args,0), get(args,1), get(args,2), get(args,3), get(args,4), get(args,5), get(args,6), get(args,7), get(args,8), get(args,9), get(args,10))  }
   if(n==12){ return call12(fptr, get(args,0), get(args,1), get(args,2), get(args,3), get(args,4), get(args,5), get(args,6), get(args,7), get(args,8), get(args,9), get(args,10), get(args,11))  }
   if(n==13){ return call13(fptr, get(args,0), get(args,1), get(args,2), get(args,3), get(args,4), get(args,5), get(args,6), get(args,7), get(args,8), get(args,9), get(args,10), get(args,11), get(args,12))  }
   panic("ffi_call supports 0-12 args")
   return 0
}