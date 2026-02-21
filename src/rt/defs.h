#ifdef _WIN32
#ifdef __argc
#undef __argc
#endif
#ifdef __argv
#undef __argv
#endif
#endif

RT_DEF("__malloc", __malloc, 1, "fn __malloc(n)",
       "Allocates n bytes of memory on the heap.")
RT_DEF("__free", __free, 1, "fn __free(p)",
       "Frees memory previously allocated by __malloc.")
RT_DEF("__realloc", __realloc, 2, "fn __realloc(p, n)",
       "Reallocates memory to a new size.")
RT_DEF("__runtime_cleanup", __runtime_cleanup, 0, "fn __runtime_cleanup()",
       "Frees runtime-owned allocations and argument buffers at shutdown.")

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
RT_DEF("__open", __open, 3, "fn __open(path, flags, mode)",
       "Portable open(2) wrapper.")
RT_DEF("__close", __close, 1, "fn __close(fd)", "Portable close(2) wrapper.")
RT_DEF("__ioctl", __ioctl, 3, "fn __ioctl(fd, req, arg)",
       "Portable ioctl(2) wrapper.")
RT_DEF("__clock_gettime", __clock_gettime, 2, "fn __clock_gettime(clk, ts)",
       "Portable clock_gettime wrapper.")
RT_DEF("__nanosleep", __nanosleep, 1, "fn __nanosleep(ts)",
       "Portable nanosleep wrapper.")
RT_DEF("__getpid", __getpid, 0, "fn __getpid()", "Portable getpid wrapper.")
RT_DEF("__getppid", __getppid, 0, "fn __getppid()", "Portable getppid wrapper.")
RT_DEF("__getuid", __getuid, 0, "fn __getuid()", "Portable getuid wrapper.")
RT_DEF("__getgid", __getgid, 0, "fn __getgid()", "Portable getgid wrapper.")
RT_DEF("__getcwd", __getcwd, 2, "fn __getcwd(buf, size)",
       "Portable getcwd wrapper.")
RT_DEF("__access", __access, 2, "fn __access(path, mode)",
       "Portable access wrapper.")
RT_DEF("__unlink", __unlink, 1, "fn __unlink(path)", "Portable unlink wrapper.")
RT_DEF("__pipe", __pipe, 1, "fn __pipe(fds_ptr)", "Portable pipe wrapper.")
RT_DEF("__dup2", __dup2, 2, "fn __dup2(oldfd, newfd)", "Portable dup2 wrapper.")
RT_DEF("__fork", __fork, 0, "fn __fork()",
       "Portable fork wrapper (non-Windows).")
RT_DEF("__wait4", __wait4, 3, "fn __wait4(pid, status_ptr, options)",
       "Portable wait4 wrapper (waitpid on non-Windows).")
RT_DEF("__spawn_wait", __spawn_wait, 2, "fn __spawn_wait(path, argv)",
       "Windows: spawn process and wait; returns exit code. Non-Windows: -1.")
RT_DEF("__spawn_pipe", __spawn_pipe, 3, "fn __spawn_pipe(path, argv, fds_ptr)",
       "Windows: spawn process with pipes; returns pid and fills fds_ptr.")
RT_DEF("__wait_process", __wait_process, 1, "fn __wait_process(pid)",
       "Windows: wait by pid and return exit code. Non-Windows: -1.")
RT_DEF("__exit", __exit, 1, "fn __exit(code)", "Portable exit wrapper.")
RT_DEF("__enable_vt", __enable_vt, 0, "fn __enable_vt()",
       "Enable VT escape processing on Windows.")
RT_DEF("__tty_raw", __tty_raw, 1, "fn __tty_raw(enable)",
       "Set stdin terminal raw mode when enable=1; restore cooked mode when 0.")
RT_DEF("__tty_pending", __tty_pending, 0, "fn __tty_pending()",
       "Returns pending stdin byte count for tty input (0 when none).")
RT_DEF("__tty_size", __tty_size, 1, "fn __tty_size(out_ptr)",
       "Writes tty cols/rows (int32,int32) to out_ptr; returns 0 on success.")
RT_DEF("__is_dir", __is_dir, 1, "fn __is_dir(path)",
       "Portable directory check.")
RT_DEF("__dir_open", __dir_open, 1, "fn __dir_open(path)",
       "Open directory handle.")
RT_DEF("__dir_read", __dir_read, 1, "fn __dir_read(handle)",
       "Read next directory entry.")
RT_DEF("__dir_close", __dir_close, 1, "fn __dir_close(handle)",
       "Close directory handle.")
RT_DEF("__socket", __socket, 3, "fn __socket(domain, type, protocol)",
       "Portable socket wrapper.")
RT_DEF("__connect", __connect, 3, "fn __connect(fd, addr, addrlen)",
       "Portable connect wrapper.")
RT_DEF("__bind", __bind, 3, "fn __bind(fd, addr, addrlen)",
       "Portable bind wrapper.")
RT_DEF("__listen", __listen, 2, "fn __listen(fd, backlog)",
       "Portable listen wrapper.")
RT_DEF("__accept", __accept, 3, "fn __accept(fd, addr, addrlen)",
       "Portable accept wrapper.")
RT_DEF("__sendto", __sendto, 6,
       "fn __sendto(fd, buf, len, flags, addr, addrlen)",
       "Portable sendto wrapper.")
RT_DEF("__recvfrom", __recvfrom, 6,
       "fn __recvfrom(fd, buf, len, flags, addr, addrlen)",
       "Portable recvfrom wrapper.")
RT_DEF("__setsockopt", __setsockopt, 5,
       "fn __setsockopt(fd, level, optname, optval, optlen)",
       "Portable setsockopt wrapper.")
RT_DEF("__recv", __recv, 4, "fn __recv(fd, buf, len, flags)",
       "Portable recv wrapper.")
RT_DEF("__send", __send, 4, "fn __send(fd, buf, len, flags)",
       "Portable send wrapper.")
RT_DEF("__closesocket", __closesocket, 1, "fn __closesocket(fd)",
       "Portable socket close wrapper.")
RT_DEF("__syscall", __syscall, 7, "fn __syscall(n, a1, a2, a3, a4, a5, a6)",
       "Executes a raw Linux system call.")
RT_DEF("__execve", __execve, 3, "fn __execve(path, argv, envp)",
       "Standard execve(2) replacement.")

RT_DEF("__tag", __tag, 1, "fn __tag(v)", "Tags a raw integer.")
RT_DEF("__untag", __untag, 1, "fn __untag(v)", "Untags a Nytrix value.")
RT_DEF("__is_int", __is_int, 1, "fn __is_int(v)",
       "Checks if value is a tagged integer.")
RT_DEF("__is_ptr", __is_ptr, 1, "fn __is_ptr(v)",
       "Checks if value is a valid pointer.")
RT_DEF("__is_ny_obj", __is_ny_obj, 1, "fn __is_ny_obj(v)",
       "Checks if value is a Nytrix heap object.")
RT_DEF("__is_str_obj", __is_str_obj, 1, "fn __is_str_obj(v)",
       "Checks if value is a Nytrix string object.")
RT_DEF("__is_float_obj", __is_float_obj, 1, "fn __is_float_obj(v)",
       "Checks if value is a Nytrix float object.")
RT_DEF("__tagof", __tagof, 1, "fn __tagof(v)",
       "Returns the raw runtime tag stored at v-8, or 0.")
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
RT_DEF("__call1_i64", __call1_i64, 2, "fn __call1_i64(fptr, a)",
       "Call fptr with one i64 argument and i64 return.")
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
RT_DEF("__trace_loc", __trace_loc, 3, "fn __trace_loc(file, line, col)",
       "Internal: record the last executed source location.")
RT_DEF("__trace_func", __trace_func, 1, "fn __trace_func(name)",
       "Internal: record the current function name.")
RT_DEF("__trace_dump", __trace_dump, 1, "fn __trace_dump(n)",
       "Internal: dump recent trace entries.")
RT_DEF("__push_defer", __push_defer, 2, "fn __push_defer(f, e)",
       "Internal: push defer.")
RT_DEF("__pop_run_defer", __pop_run_defer, 0, "fn __pop_run_defer()",
       "Internal: pop and run one defer.")
RT_DEF("__run_defers_to", __run_defers_to, 1, "fn __run_defers_to(n)",
       "Internal: run defers.")

RT_DEF("__argc", ny_rt_argc, 0, "fn __argc()",
       "Returns the number of command-line arguments.")
RT_DEF("__argvp", ny_rt_argvp, 0, "fn __argvp()",
       "Returns the raw argv pointer.")
RT_DEF("__argv", ny_rt_argv, 1, "fn __argv(i)",
       "Returns the command-line argument string at index i.")
RT_DEF("__envc", __envc, 0, "fn __envc()",
       "Returns the number of environment variables.")
RT_DEF("__envp", __envp, 0, "fn __envp()",
       "Returns the raw environment variables pointer.")
RT_DEF("__errno", __errno, 0, "fn __errno()", "Returns the last error number.")
RT_DEF("__result_ok", __result_ok, 1, "fn __result_ok(v)",
       "Creates an Ok result.")
RT_DEF("__result_err", __result_err, 1, "fn __result_err(e)",
       "Creates an Err result.")
RT_DEF("__is_ok", __is_ok, 1, "fn __is_ok(v)",
       "Checks if value is an Ok result.")
RT_DEF("__is_err", __is_err, 1, "fn __is_err(v)",
       "Checks if value is an Err result.")
RT_DEF("__unwrap", __unwrap, 1, "fn __unwrap(v)",
       "Unwraps a Result or returns the value.")

RT_DEF("__thread_spawn", __thread_spawn, 2, "fn __thread_spawn(fn, arg)",
       "Spawns a new thread.")
RT_DEF("__thread_spawn_call", __thread_spawn_call, 3,
       "fn __thread_spawn_call(fn, argc, argv)",
       "Spawns a new thread and invokes fn with argc arguments from argv.")
RT_DEF("__thread_launch_call", __thread_launch_call, 3,
       "fn __thread_launch_call(fn, argc, argv)",
       "Launches a detached thread and invokes fn with argc arguments.")
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
RT_GV("std.core.primitives.__argv_ptr", __argv_ptr, int64_t *,
      "Global: argv pointer.")
RT_GV("std.core.primitives.__envp_ptr", __envp_ptr, int64_t *,
      "Global: envp pointer.")
RT_GV("std.core.primitives.__errno_val", __errno_val, int64_t,
      "Global: errno value.")
RT_GV("std.core.primitives.__globals_ptr", g_globals_ptr, int64_t,
      "Global: globals table pointer.")

#undef RT_GV
