use std.core

#include "etc/tests/rt/ffi/fficonsts.h" as ""
#include "etc/tests/rt/ffi/transitive.h" as ""
#include <stdlib.h> as "getenv"
#windows {
   #include <windows.h> as "GetCurrentProcess"
} #else {
   #include <sys/types.h> as ""
   #include <unistd.h> as "get"
   #include <sys/time.h> as ""
} #endif
print("Testing FFI include resolution...")
assert(NYTRIX_FFI_CONST_HEX == 42, "FFI exposes object-like integer macros")
assert(NYTRIX_FFI_CONST_SHIFT == 32, "FFI folds shift macros")
assert(NYTRIX_FFI_CONST_MASK == 42 | 32, "FFI folds bitwise macro expressions")
assert(NYTRIX_FFI_ENUM_FIRST == 7, "FFI exposes enum constants")
assert(NYTRIX_FFI_ENUM_SECOND == 11, "FFI exposes later enum constants")
assert(NYTRIX_FFI_TRANSITIVE_OK == 123, "FFI keeps no-prefix imports scoped")
assert(__layout_size("NytrixFfiColor") == 4, "FFI imports typedef struct layouts")
assert(__layout_size("NytrixFfiImage") >= 20, "FFI imports pointer-bearing structs")
def path_env = getenv("PATH")
assert(type(path_env) == "str" && path_env.len > 0, "FFI converts C string returns into Ny strings")
def ffi_color = NytrixFfiColor(1, 2, 3, 4)
assert(load_layout(ffi_color, "NytrixFfiColor", "g") == 2, "FFI layout fields are loadable")
free(ffi_color)
#windows {
   def pid = GetCurrentProcessId()
   assert(pid > 0, "windows standard header import exposes GetCurrentProcessId")
} #else {
   def pid = getpid()
   assert(pid > 0, "posix standard header import exposes getpid")
   mut timeval: tv = timeval(0, 0)
   assert(gettimeofday(&tv, NULL) == 0, "posix header import accepts NULL and timeval out pointer")
} #endif
print("✓ FFI include resolution passed")
