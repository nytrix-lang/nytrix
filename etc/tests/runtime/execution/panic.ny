use std.core

def jmp_size = __jmpbuf_size()
def jmp_align = __jmpbuf_align()
assert(jmp_size > 0, "__jmpbuf_size returns a positive integer")
assert(jmp_align > 0, "__jmpbuf_align returns a positive integer")
def env_buf = malloc(jmp_size + jmp_align)
assert(env_buf != 0, "panic env buffer allocates")
assert(__set_panic_env(env_buf) == nil, "__set_panic_env pushes an env frame")
assert(__clear_panic_env() == nil, "__clear_panic_env pops an env frame")
free(env_buf)
assert(__get_panic_val() == nil, "__get_panic_val starts empty")
mut caught = nil
try {
   panic("panic marker")
} catch e {
   caught = e
}

assert(caught == "panic marker", "try/catch receives panic payload")
assert(__get_panic_val() == "panic marker", "__get_panic_val keeps last panic payload")
def bt = __get_backtrace(0)
assert(is_list(bt), "__get_backtrace returns a list")
print("✓ runtime panic tests passed")
