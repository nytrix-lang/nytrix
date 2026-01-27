RT_DEF("__malloc", __malloc, 1, "fn __malloc(n)",
       "Allocates n bytes of memory on the heap.")
RT_DEF("__free", __free, 1, "fn __free(p)",
       "Frees memory previously allocated by __malloc.")
RT_DEF("__realloc", __realloc, 2, "fn __realloc(p, n)",
       "Reallocates memory to a new size.")

RT_DEF("__load8_idx", __load8_idx, 2, "fn __load8(p, i)",
       "Loads a single byte from memory address p + i.")
RT_DEF("__load16_idx", __load16_idx, 2, "fn __load16(p, i)",
       "Loads a 16-bit integer from memory address p + i.")
RT_DEF("__load32_idx", __load32_idx, 2, "fn __load32(p, i)",
       "Loads a 32-bit integer from memory address p + i.")
RT_DEF("__load64_idx", __load64_idx, 2, "fn __load64(p, i)",
       "Loads a 64-bit integer from memory address p + i.")

RT_DEF("__store8_idx", __store8_idx, 3, "fn __store8(p, i, v)",
       "Stores byte v at memory address p + i.")
RT_DEF("__store16_idx", __store16_idx, 3, "fn __store16(p, i, v)",
       "Stores 16-bit integer v at memory address p + i.")
RT_DEF("__store32_idx", __store32_idx, 3, "fn __store32(p, i, v)",
       "Stores 32-bit integer v at memory address p + i.")
RT_DEF("__store64_idx", __store64_idx, 3, "fn __store64(p, i, v)",
       "Stores 64-bit integer v at memory address p + i.")

RT_DEF("__sys_read_off", __sys_read_off, 4, "fn __sys_read_off(fd, p, n, i)",
       "Reads n bytes from fd into address p + i.")
RT_DEF("__sys_write_off", __sys_write_off, 4, "fn __sys_write_off(fd, p, n, i)",
       "Writes n bytes to fd from address p + i.")
RT_DEF("__syscall", __syscall, 7, "fn __syscall(n, a1, a2, a3, a4, a5, a6)",
       "Executes a raw Linux system call.")
RT_DEF("__execve", __execve, 3, "fn __execve(path, argv, envp)",
       "Standard execve(2) replacement.")

RT_DEF("__add", __add, 2, "fn __add(a, b)", "Integer addition.")
RT_DEF("__sub", __sub, 2, "fn __sub(a, b)", "Integer subtraction.")
RT_DEF("__mul", __mul, 2, "fn __mul(a, b)", "Integer multiplication.")
RT_DEF("__div", __div, 2, "fn __div(a, b)", "Integer division.")
RT_DEF("__mod", __mod, 2, "fn __mod(a, b)", "Integer modulus.")
RT_DEF("__and", __and, 2, "fn __and(a, b)", "Bitwise AND.")
RT_DEF("__or", __or, 2, "fn __or(a, b)", "Bitwise OR.")
RT_DEF("__xor", __xor, 2, "fn __xor(a, b)", "Bitwise XOR.")
RT_DEF("__shl", __shl, 2, "fn __shl(a, b)", "Bitwise shift left.")
RT_DEF("__shr", __shr, 2, "fn __shr(a, b)", "Bitwise shift right.")
RT_DEF("__not", __not, 1, "fn __not(a)", "Bitwise NOT.")

RT_DEF("__eq", __eq, 2, "fn __eq(a, b)", "Integer equality.")
RT_DEF("__lt", __lt, 2, "fn __lt(a, b)", "Integer less than.")
RT_DEF("__le", __le, 2, "fn __le(a, b)", "Integer less than or equal.")
RT_DEF("__gt", __gt, 2, "fn __gt(a, b)", "Integer greater than.")
RT_DEF("__ge", __ge, 2, "fn __ge(a, b)", "Integer greater than or equal.")

RT_DEF("__flt_box_val", __flt_box_val, 1, "fn __flt_box_val(f)",
       "Boxes a raw float into a Nytrix object.")
RT_DEF("__flt_unbox_val", __flt_unbox_val, 1, "fn __flt_unbox_val(v)",
       "Unboxes a Nytrix float object to raw float.")
RT_DEF("__flt_add", __flt_add, 2, "fn __flt_add(a, b)", "Float addition.")
RT_DEF("__flt_sub", __flt_sub, 2, "fn __flt_sub(a, b)", "Float subtraction.")
RT_DEF("__flt_mul", __flt_mul, 2, "fn __flt_mul(a, b)", "Float multiplication.")
RT_DEF("__flt_div", __flt_div, 2, "fn __flt_div(a, b)", "Float division.")
RT_DEF("__flt_lt", __flt_lt, 2, "fn __flt_lt(a, b)", "Float less than.")
RT_DEF("__flt_gt", __flt_gt, 2, "fn __flt_gt(a, b)", "Float greater than.")
RT_DEF("__flt_eq", __flt_eq, 2, "fn __flt_eq(a, b)", "Float equality.")
RT_DEF("__flt_from_int", __flt_from_int, 1, "fn __flt_from_int(i)",
       "Convert int to float.")
RT_DEF("__flt_to_int", __flt_to_int, 1, "fn __flt_to_int(f)",
       "Convert float to int.")
RT_DEF("__flt_trunc", __flt_trunc, 1, "fn __flt_trunc(f)",
       "Truncate float to int.")

RT_DEF("__dlsym", __dlsym, 2, "fn __dlsym(handle, symbol)",
       "Resolves a symbol in a library.")
RT_DEF("__dlopen", __dlopen, 2, "fn __dlopen(path, flags)",
       "Opens a dynamic library.")
RT_DEF("__dlclose", __dlclose, 1, "fn __dlclose(handle)",
       "Closes a dynamic library.")
RT_DEF("__dlerror", __dlerror, 0, "fn __dlerror()",
       "Returns the last dynamic linking error.")

RT_DEF("__call0", __call0, 1, "fn __call0(fptr)", "Call fptr with 0 args.")
RT_DEF("__call1", __call1, 2, "fn __call1(fptr, a)", "Call fptr with 1 arg.")
RT_DEF("__call2", __call2, 3, "fn __call2(fptr, a, b)",
       "Call fptr with 2 args.")
RT_DEF("__call3", __call3, 4, "fn __call3(fptr, a, b, c)",
       "Call fptr with 3 args.")
RT_DEF("__call4", __call4, 5, "fn __call4(fptr, a, b, c, d)",
       "Call fptr with 4 args.")
RT_DEF("__call5", __call5, 6, "fn __call5(fptr, a, b, c, d, e)",
       "Call fptr with 5 args.")
RT_DEF("__call6", __call6, 7, "fn __call6(fptr, a, b, c, d, e, f)",
       "Call fptr with 6 args.")
RT_DEF("__call7", __call7, 8, "fn __call7(fptr, ...)", "Call fptr with 7 args.")
RT_DEF("__call8", __call8, 9, "fn __call8(fptr, ...)", "Call fptr with 8 args.")
RT_DEF("__call9", __call9, 10, "fn __call9(fptr, ...)",
       "Call fptr with 9 args.")
RT_DEF("__call10", __call10, 11, "fn __call10(fptr, ...)",
       "Call fptr with 10 args.")
RT_DEF("__call11", __call11, 12, "fn __call11(fptr, ...)",
       "Call fptr with 11 args.")
RT_DEF("__call12", __call12, 13, "fn __call12(fptr, ...)",
       "Call fptr with 12 args.")
RT_DEF("__call13", __call13, 14, "fn __call13(fptr, ...)",
       "Call fptr with 13 args.")
RT_DEF("__call14", __call14, 15, "fn __call14(fptr, ...)",
       "Call fptr with 14 args.")
RT_DEF("__call15", __call15, 16, "fn __call15(fptr, ...)",
       "Call fptr with 15 args.")

RT_DEF("__set_args", __set_args, 3, "fn __set_args(ac, av, ep)",
       "Initialize command line arguments.")
RT_DEF("__parse_ast", __parse_ast, 1, "fn __parse_ast(s)",
       "Parses a string into an AST object.")
RT_DEF("__globals", __globals, 0, "fn __globals()",
       "Returns the pointer to the global variables table.")
RT_DEF("__set_globals", __set_globals, 1, "fn __set_globals(p)",
       "Sets the pointer to the global variables table.")
RT_DEF("__panic", __panic, 1, "fn __panic(msg)", "Panics with a message.")
RT_DEF("__set_panic_env", __set_panic_env, 1, "fn __set_panic_env(e)",
       "Internal: sets panic jump environment.")
RT_DEF("__clear_panic_env", __clear_panic_env, 0, "fn __clear_panic_env()",
       "Internal: clears panic jump environment.")
RT_DEF("__jmpbuf_size", __jmpbuf_size, 0, "fn __jmpbuf_size()",
       "Internal: returns size of jmp_buf.")
RT_DEF("__get_panic_val", __get_panic_val, 0, "fn __get_panic_val()",
       "Internal: returns the panic message.")
RT_DEF("__push_defer", __push_defer, 2, "fn __push_defer(f, e)",
       "Internal: push defer.")
RT_DEF("__pop_run_defer", __pop_run_defer, 0, "fn __pop_run_defer()",
       "Internal: pop and run one defer.")
RT_DEF("__run_defers_to", __run_defers_to, 1, "fn __run_defers_to(n)",
       "Internal: run defers.")

RT_DEF("__argc", __argc, 0, "fn __argc()",
       "Returns the number of command-line arguments.")
RT_DEF("__argv", __argv, 1, "fn __argv(i)",
       "Returns the command-line argument string at index i.")
RT_DEF("__envc", __envc, 0, "fn __envc()",
       "Returns the number of environment variables.")
RT_DEF("__envp", __envp, 0, "fn __envp()",
       "Returns the raw environment variables pointer.")
RT_DEF("__errno", __errno, 0, "fn __errno()", "Returns the last error number.")

RT_DEF("__thread_spawn", __thread_spawn, 2, "fn __thread_spawn(fn, arg)",
       "Spawns a new thread.")
RT_DEF("__thread_join", __thread_join, 1, "fn __thread_join(t)",
       "Joins a thread.")
RT_DEF("__mutex_new", __mutex_new, 0, "fn __mutex_new()",
       "Creates a new mutex.")
RT_DEF("__mutex_lock64", __mutex_lock64, 1, "fn __mutex_lock64(m)",
       "Locks a mutex.")
RT_DEF("__mutex_unlock64", __mutex_unlock64, 1, "fn __mutex_unlock64(m)",
       "Unlocks a mutex.")
RT_DEF("__mutex_free", __mutex_free, 1, "fn __mutex_free(m)", "Frees a mutex.")

RT_DEF("__to_str", __to_str, 1, "fn __to_str(v)",
       "Converts primitive to string.")
RT_DEF("__str_concat", __str_concat, 2, "fn __str_concat(a, b)",
       "Concatenates two strings.")
RT_DEF("__memcpy", __memcpy, 3, "fn __memcpy(d, s, n)",
       "Copies n bytes from s to d.")
RT_DEF("__memcmp", __memcmp, 3, "fn __memcmp(a, b, n)",
       "Compares n bytes of a and b.")
RT_DEF("__memset", __memset, 3, "fn __memset(p, v, n)",
       "Sets n bytes of p to v.")
RT_DEF("__rand64", __rand64, 0, "fn __rand64()",
       "Returns a random 64-bit integer.")
RT_DEF("__srand", __srand, 1, "fn __srand(s)",
       "Seeds the random number generator.")
RT_DEF("__copy_mem", __copy_mem, 3, "fn __copy_mem(d, s, n)",
       "Copies n bytes from s to d (llvm intrinsic).")
RT_DEF("__os_name", __os_name, 0, "fn __os_name()",
       "Returns the name of the operating system.")
RT_DEF("__arch_name", __arch_name, 0, "fn __arch_name()",
       "Returns the name of the architecture.")

#ifndef RT_GV
#define RT_GV(n, p, t, d)
#endif

RT_GV("std.core.primitives.__argc_val", __argc_val, int64_t,
      "Global: argc value.")
RT_GV("std.core.primitives.__envc_val", __envc_val, int64_t,
      "Global: envc value.")
RT_GV("std.core.primitives.__argv_ptr", __argv_ptr, char **,
      "Global: argv pointer.")
RT_GV("std.core.primitives.__envp_ptr", __envp_ptr, char **,
      "Global: envp pointer.")
RT_GV("std.core.primitives.__errno_val", __errno_val, int64_t,
      "Global: errno value.")
RT_GV("std.core.primitives.__globals_ptr", g_globals_ptr, int64_t,
      "Global: globals table pointer.")

#undef RT_GV
