#ifdef _WIN32
#ifdef rt_argc
#undef rt_argc
#endif
#ifdef rt_argv
#undef rt_argv
#endif
#endif

RT_DEF("__malloc", rt_malloc, 1, "fn __malloc(n)", "Allocates n bytes of memory on the heap.")
RT_DEF("__free", rt_free, 1, "fn __free(p)", "Frees memory previously allocated by rt_malloc.")
RT_DEF("__malloc_raw", rt_malloc_raw, 1, "fn __malloc_raw(n)", "Allocates unmanaged raw bytes.")
RT_DEF("__free_raw", rt_free_raw, 1, "fn __free_raw(p)", "Frees unmanaged raw bytes.")
RT_DEF("__drop_owned", rt_drop_owned, 1, "fn __drop_owned(p)",
       "Drops an owned heap value if it is currently heap-backed.")
RT_DEF("__drop_owned_slot", rt_drop_owned_slot, 1, "fn __drop_owned_slot(slot)",
       "Drops the heap value currently stored in an owned stack slot.")
RT_DEF("__retain_owned", rt_retain_owned, 1, "fn __retain_owned(p)",
       "Retains an owned heap value in RC heap mode.")
RT_DEF("__release_owned", rt_release_owned, 1, "fn __release_owned(p)",
       "Releases an owned heap value in RC heap mode, freeing it when the count reaches zero.")
RT_DEF("__rc_count", rt_rc_count, 1, "fn __rc_count(p)",
       "Returns the current RC count for a heap value, for tests and debugging.")
RT_DEF("__realloc", rt_realloc, 2, "fn __realloc(p, n)", "Reallocates memory to a new size.")
RT_DEF("__runtime_cleanup", rt_runtime_cleanup, 0, "fn __runtime_cleanup()",
       "Frees runtime-owned allocations and argument buffers at shutdown.")

RT_DEF("__load8_idx", rt_load8_idx, 2, "fn __load8(p, i)",
       "Loads a single byte from memory address p + i.")
RT_DEF("__load16_idx", rt_load16_idx, 2, "fn __load16(p, i)",
       "Loads a 16-bit integer from memory address p + i.")
RT_DEF("__load32_idx", rt_load32_idx, 2, "fn __load32(p, i)",
       "Loads a 32-bit integer from memory address p + i.")
RT_DEF("__load32_h", rt_load32_h, 2, "fn __load32_h(p, i)",
       "Loads a 32-bit value and tags it for FFI compatibility.")
RT_DEF("__load64_idx", rt_load64_idx, 2, "fn __load64(p, i)",
       "Loads a 64-bit integer from memory address p + i.")
RT_DEF("__load64_h", rt_load64_h, 2, "fn __load64_h(p, i)",
       "Loads a 64-bit value and tags it for FFI compatibility.")

RT_DEF("__store8_idx", rt_store8_idx, 3, "fn __store8(p, i, v)",
       "Stores byte v at memory address p + i.")
RT_DEF("__store16_idx", rt_store16_idx, 3, "fn __store16(p, i, v)",
       "Stores 16-bit integer v at memory address p + i.")
RT_DEF("__store32_idx", rt_store32_idx, 3, "fn __store32(p, i, v)",
       "Stores 32-bit integer v at memory address p + i.")
RT_DEF("__store64_idx", rt_store64_idx, 3, "fn __store64(p, i, v)",
       "Stores 64-bit integer v at memory address p + i.")
RT_DEF("__store64_h", rt_store64_h, 3, "fn __store64_h(p, i, v)",
       "Untags integer v and stores it as a 64-bit handle/value.")

RT_DEF("__atomic_load64", rt_atomic_load64, 2, "fn __atomic_load64(p, i)",
       "Atomically loads a 64-bit Ny value from address p + i.")
RT_DEF("__atomic_store64", rt_atomic_store64, 3, "fn __atomic_store64(p, i, v)",
       "Atomically stores a 64-bit Ny value at address p + i.")
RT_DEF("__atomic_add64", rt_atomic_add64, 3, "fn __atomic_add64(p, i, delta)",
       "Atomically adds integer delta to the Ny integer slot at address p + i and returns the old value.")
RT_DEF("__atomic_sub64", rt_atomic_sub64, 3, "fn __atomic_sub64(p, i, delta)",
       "Atomically subtracts integer delta from the Ny integer slot at address p + i and returns the old value.")
RT_DEF("__atomic_exchange64", rt_atomic_exchange64, 3, "fn __atomic_exchange64(p, i, v)",
       "Atomically exchanges the 64-bit Ny value at address p + i and returns the old value.")
RT_DEF("__atomic_cas64", rt_atomic_cas64, 4, "fn __atomic_cas64(p, i, expected, desired)",
       "Atomically compares and swaps the 64-bit Ny value at address p + i.")

RT_DEF("__read_off", rt_read_off, 4, "fn __read_off(fd, p, n, i)",
       "Reads n bytes from fd into address p + i.")
RT_DEF("__write_off", rt_write_off, 4, "fn __write_off(fd, p, n, i)",
       "Writes n bytes to fd from address p + i.")
RT_DEF("__save_tga_rgba", rt_save_tga_rgba, 5, "fn __save_tga_rgba(path, data, w, h, channels)",
       "Writes an uncompressed top-left 32-bit TGA from an RGBA/RGB/gray byte buffer.")
RT_DEF("__open", rt_open, 3, "fn __open(path, flags, mode)", "Portable open(2) wrapper.")
RT_DEF("__close", rt_close, 1, "fn __close(fd)", "Portable close(2) wrapper.")
RT_DEF("__ioctl", rt_ioctl, 3, "fn __ioctl(fd, req, arg)", "Portable ioctl(2) wrapper.")
RT_DEF("__clock_gettime", rt_clock_gettime, 2, "fn __clock_gettime(clk, ts)",
       "Portable clock_gettime wrapper.")
RT_DEF("__nanosleep", rt_nanosleep, 1, "fn __nanosleep(ts)", "Portable nanosleep wrapper.")
RT_DEF("__time_seconds", rt_time_seconds, 0, "fn __time_seconds()", "Returns Unix seconds.")
RT_DEF("__time_milliseconds", rt_time_milliseconds, 0, "fn __time_milliseconds()", "Returns Unix milliseconds.")
RT_DEF("__ticks_ns", rt_ticks_ns, 0, "fn __ticks_ns()", "Returns monotonic nanoseconds.")
RT_DEF("__msleep_ms", rt_msleep_ms, 1, "fn __msleep_ms(ms)", "Sleeps for milliseconds.")
RT_DEF("__getpid", rt_getpid, 0, "fn __getpid()", "Portable getpid wrapper.")
RT_DEF("__getppid", rt_getppid, 0, "fn __getppid()", "Portable getppid wrapper.")
RT_DEF("__getuid", rt_getuid, 0, "fn __getuid()", "Portable getuid wrapper.")
RT_DEF("__getgid", rt_getgid, 0, "fn __getgid()", "Portable getgid wrapper.")
RT_DEF("__getcwd", rt_getcwd, 2, "fn __getcwd(buf, size)", "Portable getcwd wrapper.")
RT_DEF("__access", rt_access, 2, "fn __access(path, mode)", "Portable access wrapper.")
RT_DEF("__unlink", rt_unlink, 1, "fn __unlink(path)", "Portable unlink wrapper.")
RT_DEF("__rename", rt_rename, 2, "fn __rename(old_path, new_path)", "Portable rename wrapper.")
RT_DEF("__pipe", rt_pipe, 1, "fn __pipe(fds_ptr)", "Portable pipe wrapper.")
RT_DEF("__dup2", rt_dup2, 2, "fn __dup2(oldfd, newfd)", "Portable dup2 wrapper.")
RT_DEF("__fork", rt_fork, 0, "fn __fork()", "Portable fork wrapper (non-Windows).")
RT_DEF("__setsid", rt_setsid, 0, "fn __setsid()", "Portable setsid wrapper (non-Windows).")
RT_DEF("__openpty", rt_openpty, 1, "fn __openpty(fds_ptr)",
       "Portable openpty wrapper (non-Windows).")
RT_DEF("__wait4", rt_wait4, 3, "fn __wait4(pid, status_ptr, options)",
       "Portable wait4 wrapper (waitpid on non-Windows).")
RT_DEF("__spawn_wait", rt_spawn_wait, 2, "fn __spawn_wait(path, argv)",
       "Windows: spawn process and wait; returns exit code. Non-Windows: -1.")
RT_DEF("__spawn_pipe", rt_spawn_pipe, 3, "fn __spawn_pipe(path, argv, fds_ptr)",
       "Windows: spawn process with pipes; returns pid and fills fds_ptr.")
RT_DEF("__wait_process", rt_wait_process, 1, "fn __wait_process(pid)",
       "Windows: wait by pid and return exit code. Non-Windows: -1.")
RT_DEF("__exit", rt_exit, 1, "fn __exit(code)", "Portable exit wrapper.")
RT_DEF("__enable_vt", rt_enable_vt, 0, "fn __enable_vt()",
       "Enable VT escape processing on Windows.")
RT_DEF("__tty_raw", rt_tty_raw, 1, "fn __tty_raw(enable)",
       "Set stdin terminal raw mode when enable=1; restore cooked mode when 0.")
RT_DEF("__tty_sane_fd", rt_tty_sane_fd, 1, "fn __tty_sane_fd(fd)",
       "Set cooked/sane termios flags on the given fd when possible.")
RT_DEF("__tty_pending", rt_tty_pending, 0, "fn __tty_pending()",
       "Returns pending stdin byte count for tty input (0 when none).")
RT_DEF("__tty_size", rt_tty_size, 1, "fn __tty_size(out_ptr)",
       "Writes tty cols/rows (int32,int32) to out_ptr; returns 0 on success.")
RT_DEF("__is_dir", rt_is_dir, 1, "fn __is_dir(path)", "Portable directory check.")

RT_DEF("__dir_open", rt_dir_open, 1, "fn __dir_open(path)", "Open directory handle.")
RT_DEF("__dir_read", rt_dir_read, 1, "fn __dir_read(handle)", "Read next directory entry.")
RT_DEF("__dir_close", rt_dir_close, 1, "fn __dir_close(handle)", "Close directory handle.")

RT_DEF("__inotify_init", rt_inotify_init, 1, "fn __inotify_init(flags)", "inotify_init1 or inotify_init wrapper for file watching.")
RT_DEF("__inotify_add_watch", rt_inotify_add_watch, 3, "fn __inotify_add_watch(fd, path, mask)", "Add inotify watch for path with mask.")
RT_DEF("__inotify_rm_watch", rt_inotify_rm_watch, 2, "fn __inotify_rm_watch(fd, wd)", "Remove inotify watch.")

RT_DEF("__kqueue", rt_kqueue, 0, "fn __kqueue()", "Create a kqueue (macOS/BSD) for file watching.")
RT_DEF("__kevent", rt_kevent, 7, "fn __kevent(kq, fd, filter, flags, fflags, data, udata)", "kevent call for registering/reading vnode events.")
RT_DEF("__kqueue_close", rt_kqueue_close, 1, "fn __kqueue_close(kq)", "Close kqueue fd.")
RT_DEF("__watch_open_vnode", rt_watch_open_vnode, 1, "fn __watch_open_vnode(path)", "Open a file/dir fd suitable for kqueue vnode watching (macOS).")

RT_DEF("__win32_find_first_change", rt_win32_find_first_change, 3, "fn __win32_find_first_change(path, watch_subtree, filter)", "Windows FindFirstChangeNotification.")
RT_DEF("__win32_find_next_change", rt_win32_find_next_change, 1, "fn __win32_find_next_change(handle)", "Windows FindNextChangeNotification.")
RT_DEF("__win32_find_close_change", rt_win32_find_close_change, 1, "fn __win32_find_close_change(handle)", "Windows FindCloseChangeNotification.")

RT_DEF("__big_add_abs", rt_big_add_abs, 2, "fn __big_add_abs(a, b)",
       "Internal: Native absolute BigInt addition.")
RT_DEF("__big_sub_abs", rt_big_sub_abs, 2, "fn __big_sub_abs(a, b)",
       "Internal: Native absolute BigInt subtraction.")
RT_DEF("__big_mul_abs", rt_big_mul_abs, 2, "fn __big_mul_abs(a, b)",
       "Internal: Native absolute BigInt multiplication.")
RT_DEF("__bigint_add", rt_bigint_add, 2, "fn __bigint_add(a, b)",
       "Adds two BigInt values using the runtime bigint implementation.")
RT_DEF("__bigint_sub", rt_bigint_sub, 2, "fn __bigint_sub(a, b)",
       "Subtracts two BigInt values using the runtime bigint implementation.")
RT_DEF("__bigint_mul", rt_bigint_mul, 2, "fn __bigint_mul(a, b)",
       "Multiplies two BigInt values using the runtime bigint implementation.")
RT_DEF("__bigint_submul", rt_bigint_submul, 3, "fn __bigint_submul(a, q, b)",
       "Computes a - q*b for BigInt-heavy arithmetic without materializing the product.")
RT_DEF("__bigint_row_submul", rt_bigint_row_submul, 4, "fn __bigint_row_submul(row_k, row_j, q, limit)",
       "Applies row_k[c] = row_k[c] - q*row_j[c] for c <= limit.")
RT_DEF("__bigint_row_submul_auto", rt_bigint_row_submul_auto, 3,
       "fn __bigint_row_submul_auto(row_k, row_j, q)",
       "Applies row_k -= q*row_j after scanning row_j's active tail.")
RT_DEF("__bigint_cmp", rt_bigint_cmp, 2, "fn __bigint_cmp(a, b)",
       "Compares two BigInt values using the runtime bigint implementation.")
RT_DEF("__bigint_div", rt_bigint_div, 2, "fn __bigint_div(a, b)",
       "Divides two BigInt values using the runtime bigint implementation.")
RT_DEF("__bigint_mod", rt_bigint_mod, 2, "fn __bigint_mod(a, b)",
       "Computes a modulo b using the runtime bigint implementation.")
RT_DEF("__bigint_or", rt_bigint_or, 2, "fn __bigint_or(a, b)",
       "Computes bitwise OR for non-negative BigInts using the runtime bigint implementation.")
RT_DEF("__bigint_xor", rt_bigint_xor, 2, "fn __bigint_xor(a, b)",
       "Computes bitwise XOR for non-negative BigInts using the runtime bigint implementation.")
RT_DEF("__bigint_pow", rt_bigint_pow, 2, "fn __bigint_pow(a, b)",
       "Raises a BigInt to a non-negative BigInt exponent using the runtime "
       "implementation.")
RT_DEF("__bigint_powmod", rt_bigint_powmod, 3, "fn __bigint_powmod(a, b, m)",
       "Computes modular exponentiation for BigInts using the runtime "
       "implementation.")
RT_DEF("__bigint_modinv", rt_bigint_modinv, 2, "fn __bigint_modinv(a, m)",
       "Computes a modular inverse for BigInts using the runtime implementation.")
RT_DEF("__bigint_isqrt", rt_bigint_isqrt, 1, "fn __bigint_isqrt(a)",
       "Computes the integer square root for a BigInt using the runtime "
       "implementation.")
RT_DEF("__bigint_to_int", rt_bigint_to_int, 1, "fn __bigint_to_int(a)",
       "Converts a BigInt to a tagged integer.")
RT_DEF("__bigint_to_f64", rt_bigint_to_f64, 1, "fn __bigint_to_f64(a)",
       "Converts a BigInt to an approximate f64.")
RT_DEF("__bigint_f64buf_store", rt_bigint_f64buf_store, 3, "fn __bigint_f64buf_store(buf, i, a)",
       "Stores an int/BigInt value into a raw f64 buffer slot.")
RT_DEF("__bigint_from_int", rt_bigint_from_int, 1, "fn __bigint_from_int(a)",
       "Converts a tagged integer to a BigInt.")
RT_DEF("__long", rt_long, 1, "fn __long(a)",
       "Converts ints, floats, BigInts, byte lists, strings, and bytes buffers to BigInt.")
RT_DEF("__bigint_from_str", rt_bigint_from_str, 1, "fn __bigint_from_str(s)",
       "Parses a decimal string into a BigInt.")
RT_DEF("__bigint_to_str", rt_bigint_to_str, 1, "fn __bigint_to_str(a)",
       "Converts a BigInt to a string.")
RT_DEF("__bigint_to_bytes", rt_bigint_to_bytes, 1, "fn __bigint_to_bytes(a)",
       "Converts a BigInt to a big-endian byte list.")
RT_DEF("__bigint_gcd", rt_bigint_gcd, 2, "fn __bigint_gcd(a, b)",
       "Computes GCD of two BigInts using GMP.")
RT_DEF("__bigint_legendre", rt_bigint_legendre, 2, "fn __bigint_legendre(a, p)",
       "Legendre symbol (a/p) using GMP.")
RT_DEF("__bigint_jacobi", rt_bigint_jacobi, 2, "fn __bigint_jacobi(a, n)",
       "Jacobi symbol (a/n) using GMP.")
RT_DEF("__bigint_kronecker", rt_bigint_kronecker, 2, "fn __bigint_kronecker(a, n)",
       "Kronecker symbol (a/n) using GMP.")
RT_DEF("__bigint_iroot", rt_bigint_iroot, 2, "fn __bigint_iroot(n, k)",
       "Integer k-th root of n using GMP.")
RT_DEF("__bigint_is_perfect_square", rt_bigint_is_perfect_square, 1,
       "fn __bigint_is_perfect_square(n)", "Returns 1 if n is a perfect square, 0 otherwise.")
RT_DEF("__bigint_xgcd", rt_bigint_xgcd, 2, "fn __bigint_xgcd(a, b)",
       "Extended GCD: returns [g, x, y] such that a*x + b*y = g.")
RT_DEF("__bigint_bitlen", rt_bigint_bitlen, 1, "fn __bigint_bitlen(a)",
       "Returns the number of bits needed to represent |a|. Maps to GMP "
       "mpz_sizeinbase.")
RT_DEF("__bigint_popcount", rt_bigint_popcount, 1, "fn __bigint_popcount(a)",
       "Returns the population count (number of 1-bits) of |a|. Uses GMP.")
RT_DEF("__bigint_clz", rt_bigint_clz, 1, "fn __bigint_clz(a)",
       "Count leading zeros in the most significant limb of |a|.")
RT_DEF("__bigint_ctz", rt_bigint_ctz, 1, "fn __bigint_ctz(a)",
       "Count trailing zeros in |a|. Maps to GMP mpz_scan1.")
RT_DEF("__bigint_gf2_mod", rt_bigint_gf2_mod, 2, "fn __bigint_gf2_mod(a, m)",
       "Reduce GF(2) polynomial a modulo m.")
RT_DEF("__bigint_gf2_mulmod", rt_bigint_gf2_mulmod, 3, "fn __bigint_gf2_mulmod(a, b, m)",
       "Carryless multiply a*b modulo m over GF(2).")
RT_DEF("__bigint_gf2_inv", rt_bigint_gf2_inv, 2, "fn __bigint_gf2_inv(a, m)",
       "Invert a modulo m over GF(2), returning 0 when no inverse exists.")
RT_DEF("__ct_compare", rt_ct_compare, 3, "fn __ct_compare(a, b, len)",
       "Constant-time byte buffer comparison. Returns 0 if equal, non-zero "
       "otherwise.")
RT_DEF("__ct_select", rt_ct_select, 3, "fn __ct_select(a, b, condition)",
       "Constant-time conditional select. Returns a if condition else b, "
       "without branching.")
RT_DEF("__write_buffered", rt_write_buffered, 3, "fn __write_buffered(fd, buf, len)",
       "Writes to standard output with internal buffering.")
RT_DEF("__print_flush", rt_print_flush, 0, "fn __print_flush()",
       "Explicitly flushes the internal standard output buffer.")
RT_DEF("__socket", rt_socket, 3, "fn __socket(domain, type, protocol)", "Portable socket wrapper.")
RT_DEF("__connect", rt_connect, 3, "fn __connect(fd, addr, addrlen)", "Portable connect wrapper.")
RT_DEF("__bind", rt_bind, 3, "fn __bind(fd, addr, addrlen)", "Portable bind wrapper.")
RT_DEF("__listen", rt_listen, 2, "fn __listen(fd, backlog)", "Portable listen wrapper.")
RT_DEF("__accept", rt_accept, 3, "fn __accept(fd, addr, addrlen)", "Portable accept wrapper.")
RT_DEF("__sendto", rt_sendto, 6, "fn __sendto(fd, buf, len, flags, addr, addrlen)",
       "Portable sendto wrapper.")
RT_DEF("__recvfrom", rt_recvfrom, 6, "fn __recvfrom(fd, buf, len, flags, addr, addrlen)",
       "Portable recvfrom wrapper.")
RT_DEF("__setsockopt", rt_setsockopt, 5, "fn __setsockopt(fd, level, optname, optval, optlen)",
       "Portable setsockopt wrapper.")
RT_DEF("__recv", rt_recv, 4, "fn __recv(fd, buf, len, flags)", "Portable recv wrapper.")
RT_DEF("__send", rt_send, 4, "fn __send(fd, buf, len, flags)", "Portable send wrapper.")
RT_DEF("__closesocket", rt_closesocket, 1, "fn __closesocket(fd)", "Portable socket close wrapper.")
RT_DEF("__async_task_new", rt_async_task_new, 3, "fn __async_task_new(fn, argc, argv)",
       "Creates a stackless async task from a callable and copied argument vector.")
RT_DEF("__async_value", rt_async_value, 1, "fn __async_value(value)",
       "Creates an already-completed async task with the given result.")
RT_DEF("__async_await_blocking", rt_async_await_blocking, 1, "fn __async_await_blocking(task)",
       "Drives the async scheduler until task completion and returns its result.")
RT_DEF("__async_run", rt_async_run, 1, "fn __async_run(task)",
       "Runs an async task to completion.")
RT_DEF("__async_yield", rt_async_yield, 0, "fn __async_yield()",
       "Creates an immediately-ready scheduler yield task.")
RT_DEF("__async_sleep_ms", rt_async_sleep_ms, 1, "fn __async_sleep_ms(ms)",
       "Creates a timer task that completes after the requested milliseconds.")
RT_DEF("__async_wait_fd", rt_async_wait_fd, 3, "fn __async_wait_fd(fd, events, timeout_ms)",
       "Creates a task waiting for fd readability/writability.")
RT_DEF("__async_recv", rt_async_recv, 4, "fn __async_recv(fd, buf, len, flags)",
       "Creates a socket recv task.")
RT_DEF("__async_send", rt_async_send, 4, "fn __async_send(fd, buf, len, flags)",
       "Creates a socket send task.")
RT_DEF("__async_accept", rt_async_accept, 1, "fn __async_accept(fd)",
       "Creates a socket accept task.")
RT_DEF("__async_connect", rt_async_connect, 3, "fn __async_connect(fd, addr, addrlen)",
       "Creates a non-blocking socket connect task.")
RT_DEF("__async_read_socket", rt_async_read_socket, 2, "fn __async_read_socket(fd, max_len)",
       "Creates a socket read task returning a string.")
RT_DEF("__async_write_socket_part", rt_async_write_socket_part, 4,
       "fn __async_write_socket_part(fd, data, off, size)",
       "Creates a socket write task for a string slice.")
RT_DEF("__async_write_socket_all", rt_async_write_socket_all, 2,
       "fn __async_write_socket_all(fd, data)",
       "Creates a socket write task that drains the whole string.")
RT_DEF("__async_read_socket_until", rt_async_read_socket_until, 3,
       "fn __async_read_socket_until(fd, needle, max_bytes)",
       "Creates a socket read task that completes when a delimiter is seen.")
RT_DEF("__async_state_of", rt_async_state_of, 1, "fn __async_state_of(task)",
       "Returns the internal async task state.")
RT_DEF("__syscall", rt_syscall, 7, "fn __syscall(n, a1, a2, a3, a4, a5, a6)",
       "Executes a raw Linux system call.")
RT_DEF("__execve", rt_execve, 3, "fn __execve(path, argv, envp)", "Standard execve(2) replacement.")

RT_DEF("__tag", rt_tag, 1, "fn __tag(v)", "Tags a raw integer.")
RT_DEF("__untag", rt_untag, 1, "fn __untag(v)", "Untags a Nytrix value.")
RT_DEF("__is_int", rt_is_int, 1, "fn __is_int(v)", "Checks if value is a tagged integer.")
RT_DEF("__is_ptr", rt_is_ptr, 1, "fn __is_ptr(v)", "Checks if value is a valid pointer.")
RT_DEF("__ptr_key", rt_ptr_key, 1, "fn __ptr_key(v)",
       "Formats a raw pointer address without inspecting pointee memory.")
RT_DEF("__is_ny_obj", rt_is_ny_obj, 1, "fn __is_ny_obj(v)",
       "Checks if value is a Nytrix heap object.")
RT_DEF("__is_str_obj", rt_is_str_obj, 1, "fn __is_str_obj(v)",
       "Checks if value is a Nytrix string object.")
RT_DEF("__is_float_obj", rt_is_float_obj, 1, "fn __is_float_obj(v)",
       "Checks if value is a Nytrix float object.")
RT_DEF("__is_complex_obj", rt_is_complex_obj, 1, "fn __is_complex_obj(v)",
       "Checks if value is a Nytrix complex object.")
RT_DEF("__has_tag", rt_has_tag, 2, "fn __has_tag(v, tag)",
       "Checks whether value v is a Nytrix object with runtime tag.")
RT_DEF("__tagof", rt_tagof, 1, "fn __tagof(v)", "Returns the raw runtime tag stored at v-8, or 0.")
RT_DEF("__runtime_tag", rt_runtime_tag, 1, "fn __runtime_tag(name)",
       "Returns the runtime tag integer for a named built-in Nytrix type.")
RT_DEF("__init_str", rt_init_str, 2, "fn __init_str(p, n)",
       "Initializes raw memory as a Nytrix string object.")
RT_DEF("__bytes_new", rt_bytes_new, 1, "fn __bytes_new(n)",
       "Allocates a Nytrix bytes object.")
RT_DEF("__kwarg_new", rt_kwarg_new, 2, "fn __kwarg_new(key, value)",
       "Allocates a keyword-argument wrapper object.")
RT_DEF("__range_new", rt_range_new, 3, "fn __range_new(start, stop, step)",
       "Allocates a Nytrix range object.")
RT_DEF("__list_as_tuple", rt_list_as_tuple, 1, "fn __list_as_tuple(list)",
       "Retags a list object as a tuple object.")
RT_DEF("__add", rt_add, 2, "fn __add(a, b)", "Integer addition.")
RT_DEF("__sub", rt_sub, 2, "fn __sub(a, b)", "Integer subtraction.")
RT_DEF("__mul", rt_mul, 2, "fn __mul(a, b)", "Integer multiplication.")
RT_DEF("__div", rt_div, 2, "fn __div(a, b)", "Integer division.")
RT_DEF("__mod", rt_mod, 2, "fn __mod(a, b)", "Integer modulus.")
RT_DEF("__and", rt_and, 2, "fn __and(a, b)", "Bitwise AND.")
RT_DEF("__or", rt_or, 2, "fn __or(a, b)", "Bitwise OR.")
RT_DEF("__xor", rt_xor, 2, "fn __xor(a, b)", "Bitwise XOR.")
RT_DEF("__shl", rt_shl, 2, "fn __shl(a, b)", "Bitwise shift left.")
RT_DEF("__shr", rt_shr, 2, "fn __shr(a, b)", "Bitwise shift right.")
RT_DEF("__not", rt_not, 1, "fn __not(a)", "Bitwise NOT.")
RT_DEF("__eq", rt_eq, 2, "fn __eq(a, b)", "Integer equality.")
RT_DEF("__lt", rt_lt, 2, "fn __lt(a, b)", "Integer less than.")
RT_DEF("__le", rt_le, 2, "fn __le(a, b)", "Integer less than or equal.")
RT_DEF("__gt", rt_gt, 2, "fn __gt(a, b)", "Integer greater than.")
RT_DEF("__ge", rt_ge, 2, "fn __ge(a, b)", "Integer greater than or equal.")

RT_DEF("__list_new", rt_list_new, 1, "fn __list_new(n)", "Allocates an empty list with capacity n.")
RT_DEF("__load_item", rt_load_item, 2, "fn __load_item(lst, i)", "Loads element i from list lst.")
RT_DEF("__store_item", rt_store_item, 3, "fn __store_item(lst, i, v)",
       "Stores v at element i in list lst.")
RT_DEF("__load_item_fast", rt_load_item_fast, 2, "fn __load_item_fast(lst, i)",
       "Unchecked list element load (internal hot path).")
RT_DEF("__store_item_fast", rt_store_item_fast, 3, "fn __store_item_fast(lst, i, v)",
       "Unchecked list element store (internal hot path).")
RT_DEF("__sort_list", rt_sort_list, 1, "fn __sort_list(lst)",
       "Sorts a list in place using the runtime comparator.")
RT_DEF("__sort_any", rt_sort_any, 1, "fn __sort_any(xs)",
       "Sorts a list in place or returns a sorted sequence copy.")
RT_DEF("__sorted_any", rt_sorted_any, 1, "fn __sorted_any(xs)",
       "Returns a sorted copy for supported sequence values.")
RT_DEF("__append", rt_append, 2, "fn __append(lst, v)", "Appends v to list lst.")
RT_DEF("__list_reserve", rt_list_reserve, 2, "fn __list_reserve(lst, cap)",
       "Ensures list capacity is at least cap without changing length.")
RT_DEF("__list_sum_int_range", rt_list_sum_int_range, 3, "fn __list_sum_int_range(lst, start, stop)",
       "Sums integer-like list elements over a tagged index range.")
RT_DEF("__dict_reserve", rt_dict_reserve, 2, "fn __dict_reserve(d, additional)",
       "Ensures dictionary capacity for additional expected inserts.")
RT_DEF("__dict_write_fast", rt_dict_write_fast, 3, "fn __dict_write_fast(d, k, v)",
       "Hot dictionary write helper for compiler-lowered dict.set.")
RT_DEF("__list_len", rt_list_len, 1, "fn __list_len(lst)",
       "Fast read of the element count (tagged) from a list header.")
RT_DEF("__list_set_len", rt_list_set_len, 2, "fn __list_set_len(lst, n)",
       "Fast write of the element count (tagged) into a list header.")

RT_DEF("__flt_box_val", rt_flt_box_val, 1, "fn __flt_box_val(f)",
       "Boxes a raw float into a Nytrix object.")
RT_DEF("__flt_box_val32", rt_flt_box_val32, 1, "fn __flt_box_val32(bits32)",
       "Boxes IEEE float32 bits into a Nytrix float object.")
RT_DEF("__flt_unbox_val", rt_flt_unbox_val, 1, "fn __flt_unbox_val(v)",
       "Unboxes a Nytrix float object to raw float.")
RT_DEF("__flt_unbox_val32", rt_flt_unbox_val32, 1, "fn __flt_unbox_val32(v)",
       "Unboxes a Nytrix float object to 32-bit float bits.")
RT_DEF("__flt_add", rt_flt_add, 2, "fn __flt_add(a, b)", "Float addition.")
RT_DEF("__flt_sub", rt_flt_sub, 2, "fn __flt_sub(a, b)", "Float subtraction.")
RT_DEF("__flt_mul", rt_flt_mul, 2, "fn __flt_mul(a, b)", "Float multiplication.")
RT_DEF("__flt_div", rt_flt_div, 2, "fn __flt_div(a, b)", "Float division.")
RT_DEF("__flt_lt", rt_flt_lt, 2, "fn __flt_lt(a, b)", "Float less than.")
RT_DEF("__flt_gt", rt_flt_gt, 2, "fn __flt_gt(a, b)", "Float greater than.")
RT_DEF("__flt_eq", rt_flt_eq, 2, "fn __flt_eq(a, b)", "Float equality.")
RT_DEF("__flt_is_nan", rt_flt_is_nan, 1, "fn __flt_is_nan(f)", "Returns true for NaN floats.")
RT_DEF("__flt_is_inf", rt_flt_is_inf, 1, "fn __flt_is_inf(f)", "Returns true for infinite floats.")
RT_DEF("__flt_nan", rt_flt_nan, 0, "fn __flt_nan()", "Returns a NaN float.")
RT_DEF("__flt_inf", rt_flt_inf, 0, "fn __flt_inf()", "Returns positive infinity.")
RT_DEF("__flt_hash", rt_flt_hash, 1, "fn __flt_hash(f)", "Returns a stable integer hash for a float.")
RT_DEF("__flt_sin", rt_flt_sin, 1, "fn __flt_sin(x)", "Float sine.")
RT_DEF("__flt_cos", rt_flt_cos, 1, "fn __flt_cos(x)", "Float cosine.")
RT_DEF("__flt_tan", rt_flt_tan, 1, "fn __flt_tan(x)", "Float tangent.")
RT_DEF("__flt_asin", rt_flt_asin, 1, "fn __flt_asin(x)", "Float arcsine.")
RT_DEF("__flt_acos", rt_flt_acos, 1, "fn __flt_acos(x)", "Float arccosine.")
RT_DEF("__flt_atan", rt_flt_atan, 1, "fn __flt_atan(x)", "Float arctangent.")
RT_DEF("__flt_atan2", rt_flt_atan2, 2, "fn __flt_atan2(y, x)", "Float arctangent with quadrant handling.")
RT_DEF("__flt_sqrt", rt_flt_sqrt, 1, "fn __flt_sqrt(x)", "Float square root.")
RT_DEF("__flt_exp", rt_flt_exp, 1, "fn __flt_exp(x)", "Float natural exponent.")
RT_DEF("__flt_log", rt_flt_log, 1, "fn __flt_log(x)", "Float natural logarithm.")
RT_DEF("__flt_log2", rt_flt_log2, 1, "fn __flt_log2(x)", "Float base-2 logarithm.")
RT_DEF("__flt_log10", rt_flt_log10, 1, "fn __flt_log10(x)", "Float base-10 logarithm.")
RT_DEF("__flt_floor", rt_flt_floor, 1, "fn __flt_floor(x)", "Float floor as integer.")
RT_DEF("__flt_ceil", rt_flt_ceil, 1, "fn __flt_ceil(x)", "Float ceil as integer.")
RT_DEF("__flt_round", rt_flt_round, 1, "fn __flt_round(x)", "Float round as integer.")
RT_DEF("__flt_fmod", rt_flt_fmod, 2, "fn __flt_fmod(a, b)", "Float remainder.")
RT_DEF("__flt_pow", rt_flt_pow, 2, "fn __flt_pow(a, b)", "Float power.")
RT_DEF("__flt_from_int", rt_flt_from_int, 1, "fn __flt_from_int(i)", "Convert int to float.")
RT_DEF("__flt_to_int", rt_flt_to_int, 1, "fn __flt_to_int(f)", "Convert float to int.")
RT_DEF("__flt_trunc", rt_flt_trunc, 1, "fn __flt_trunc(f)", "Truncate float to int.")
RT_DEF("__complex_new", rt_complex_new, 2, "fn __complex_new(re, im)",
       "Boxes a native complex value from real and imaginary parts.")
RT_DEF("__complex_new_bits", rt_complex_new_bits, 2, "fn __complex_new_bits(re_bits, im_bits)",
       "Boxes a native complex value from raw f64 bits.")
RT_DEF("__complex_real", rt_complex_real, 1, "fn __complex_real(z)",
       "Returns the real component of a complex value.")
RT_DEF("__complex_imag", rt_complex_imag, 1, "fn __complex_imag(z)",
       "Returns the imaginary component of a complex value.")
RT_DEF("__complex_re_bits", rt_complex_re_bits, 1, "fn __complex_re_bits(z)",
       "Returns raw f64 bits for the real component.")
RT_DEF("__complex_im_bits", rt_complex_im_bits, 1, "fn __complex_im_bits(z)",
       "Returns raw f64 bits for the imaginary component.")
RT_DEF("__complex_add", rt_complex_add, 2, "fn __complex_add(a, b)", "Complex addition.")
RT_DEF("__complex_sub", rt_complex_sub, 2, "fn __complex_sub(a, b)", "Complex subtraction.")
RT_DEF("__complex_mul", rt_complex_mul, 2, "fn __complex_mul(a, b)", "Complex multiplication.")
RT_DEF("__complex_div", rt_complex_div, 2, "fn __complex_div(a, b)", "Complex division.")
RT_DEF("__complex_conj", rt_complex_conj, 1, "fn __complex_conj(z)", "Complex conjugate.")
RT_DEF("__complex_abs2", rt_complex_abs2, 1, "fn __complex_abs2(z)", "Squared complex magnitude.")
RT_DEF("__complex_eq", rt_complex_eq, 2, "fn __complex_eq(a, b)", "Complex equality.")

RT_DEF("__dlsym", rt_dlsym, 2, "fn __dlsym(handle, symbol)", "Resolves a symbol in a library.")
RT_DEF("__dlopen", rt_dlopen, 2, "fn __dlopen(path, flags)", "Opens a dynamic library.")
RT_DEF("__dlclose", rt_dlclose, 1, "fn __dlclose(handle)", "Closes a dynamic library.")
RT_DEF("__dlerror", rt_dlerror, 0, "fn __dlerror()", "Returns the last dynamic linking error.")
RT_DEF("__zlib_uncompress", rt_zlib_uncompress, 4,
       "fn __zlib_uncompress(dest, destLen_p, src, srcLen)", "Decompresses zlib data.")
RT_DEF("__zlib_compress", rt_zlib_compress, 5,
       "fn __zlib_compress(dest, destLen_p, src, srcLen, level)", "Compresses data to zlib format.")
RT_DEF("__zlib_compress_str", rt_zlib_compress_str, 3,
       "fn __zlib_compress_str(src, srcLen, level)", "Compresses data to a Nytrix string.")
RT_DEF("__zlib_bound", rt_zlib_bound, 1, "fn __zlib_bound(n)",
       "Returns upper bound for compressed size.")
RT_DEF("__tag_native", rt_tag_native, 1, "fn __tag_native(addr)",
       "Tags a raw function pointer as a native callable.")

RT_DEF("__call0", rt_call0, 1, "fn __call0(fptr)", "Call fptr with 0 args.")
RT_DEF("__call0_void", rt_call0_void, 1, "fn __call0_void(fptr)",
       "Call fptr with no arguments and no return value.")
RT_DEF("__call0_ptr", rt_call0_ptr, 1, "fn __call0_ptr(fptr)",
       "Call fptr with 0 args and return a native pointer handle.")
RT_DEF("__call0_i32", rt_call0_i32, 1, "fn __call0_i32(fptr)",
       "Call fptr with 0 args and i32 return.")
RT_DEF("__call1", rt_call1, 2, "fn __call1(fptr, a)", "Call fptr with 1 arg.")
RT_DEF("__call1_ptr", rt_call1_ptr, 2, "fn __call1_ptr(fptr, a)",
       "Call fptr with 1 arg and return a native pointer handle.")
RT_DEF("__call1_i64", rt_call1_i64, 2, "fn __call1_i64(fptr, a)",
       "Call fptr with one i64 argument and i64 return.")
RT_DEF("__call1_u32", rt_call1_u32, 2, "fn __call1_u32(fptr, a)",
       "Call fptr with one u32 argument and u32 return.")
RT_DEF("__call1_u32_void", rt_call1_u32_void, 2, "fn __call1_u32_void(fptr, a)",
       "Call fptr with one u32 argument and no return value.")
RT_DEF("__call1_void", rt_call1_void, 2, "fn __call1_void(fptr, a)",
       "Call fptr with one argument and no return value.")
RT_DEF("__call2", rt_call2, 3, "fn __call2(fptr, a, b)", "Call fptr with 2 args.")
RT_DEF("__call2_void", rt_call2_void, 3, "fn __call2_void(fptr, a, b)",
       "Call fptr with two arguments and no return value.")
RT_DEF("__call2_ptr", rt_call2_ptr, 3, "fn __call2_ptr(fptr, a, b)",
       "Call fptr with 2 args and return a native pointer handle.")
RT_DEF("__call2_ptr_u32", rt_call2_ptr_u32, 3, "fn __call2_ptr_u32(fptr, a, b)",
       "Call fptr(ptr, u32) and return a native pointer handle.")
RT_DEF("__call3", rt_call3, 4, "fn __call3(fptr, a, b, c)", "Call fptr with 3 args.")
RT_DEF("__call3_void", rt_call3_void, 4, "fn __call3_void(fptr, a, b, c)",
       "Call fptr with three arguments and no return value.")
RT_DEF("__call3_ptr", rt_call3_ptr, 4, "fn __call3_ptr(fptr, a, b, c)",
       "Call fptr with 3 args and return a native pointer handle.")
RT_DEF("__call3_ptr_u64_ptr", rt_call3_ptr_u64_ptr, 4, "fn __call3_ptr_u64_ptr(fptr, a, b, c)",
       "Call fptr(ptr, u64, ptr) and return a native pointer handle.")
RT_DEF("__call3_ptr_u32_ptr", rt_call3_ptr_u32_ptr, 4, "fn __call3_ptr_u32_ptr(fptr, a, b, c)",
       "Call fptr(ptr, u32, ptr) and return a native pointer handle.")
RT_DEF("__call3_ptr_ptr_u32", rt_call3_ptr_ptr_u32, 4, "fn __call3_ptr_ptr_u32(fptr, a, b, c)",
       "Call fptr(ptr, ptr, u32) and return a native pointer handle.")
RT_DEF("__call4", rt_call4, 5, "fn __call4(fptr, a, b, c, d)", "Call fptr with 4 args.")
RT_DEF("__call4_void", rt_call4_void, 5, "fn __call4_void(fptr, a, b, c, d)",
       "Call fptr with four arguments and no return value.")
RT_DEF("__call4_ptr", rt_call4_ptr, 5, "fn __call4_ptr(fptr, a, b, c, d)",
       "Call fptr with 4 args and return a native pointer handle.")
RT_DEF("__call3_ptr_u64_ptr_i32", rt_call3_ptr_u64_ptr_i32, 4,
       "fn __call3_ptr_u64_ptr_i32(fptr, a, b, c)", "Call fptr(ptr, u64, ptr) and return i32.")
RT_DEF("__call4_ptr_ptr_ptr_ptr_i32", rt_call4_ptr_ptr_ptr_ptr_i32, 5,
       "fn __call4_ptr_ptr_ptr_ptr_i32(fptr, a, b, c, d)",
       "Call fptr(ptr, ptr, ptr, ptr) and return i32.")
RT_DEF("__call4_ptr_u32_u64_ptr_i32", rt_call4_ptr_u32_u64_ptr_i32, 5,
       "fn __call4_ptr_u32_u64_ptr_i32(fptr, a, b, c, d)",
       "Call fptr(ptr, u32, u64, ptr) and return i32.")
RT_DEF("__call4_ptr_u64_ptr_ptr_i32", rt_call4_ptr_u64_ptr_ptr_i32, 5,
       "fn __call4_ptr_u64_ptr_ptr_i32(fptr, a, b, c, d)",
       "Call fptr(ptr, u64, ptr, ptr) and return i32.")
RT_DEF("__call4_ptr_ptr_ptr_u64_i32", rt_call4_ptr_ptr_ptr_u64_i32, 5,
       "fn __call4_ptr_ptr_ptr_u64_i32(fptr, a, b, c, d)",
       "Call fptr(ptr, ptr, ptr, u64) and return i32.")
RT_DEF("__call1_f32_void", rt_call1_f32_void, 2, "fn __call1_f32_void(fptr, a)",
       "Call fptr with 1 float arg.")
RT_DEF("__call2_f32_void", rt_call2_f32_void, 3, "fn __call2_f32_void(fptr, a, b)",
       "Call fptr with 2 float args.")
RT_DEF("__call3_f32_void", rt_call3_f32_void, 4, "fn __call3_f32_void(fptr, a, b, c)",
       "Call fptr with 3 float args.")
RT_DEF("__call4_f32_void", rt_call4_f32_void, 5, "fn __call4_f32_void(fptr, a, b, c, d)",
       "Call fptr with 4 float args.")
RT_DEF("__call4_ptr_ptr_ptr_ptr_void", rt_call4_ptr_ptr_ptr_ptr_void, 5,
       "fn __call4_ptr_ptr_ptr_ptr_void(fptr, a, b, c, d)",
       "Call fptr(ptr, ptr, ptr, ptr) and return no value.")
RT_DEF("__call5", rt_call5, 6, "fn __call5(fptr, a, b, c, d, e)", "Call fptr with 5 args.")
RT_DEF("__call5_void", rt_call5_void, 6, "fn __call5_void(fptr, a, b, c, d, e)",
       "Call fptr with five arguments and no return value.")
RT_DEF("__call5_ptr", rt_call5_ptr, 6, "fn __call5_ptr(fptr, a, b, c, d, e)",
       "Call fptr with 5 args and return a native pointer handle.")
RT_DEF("__call5_ptr_ptr_ptr_u64_i32_i32", rt_call5_ptr_ptr_ptr_u64_i32_i32, 6,
       "fn __call5_ptr_ptr_ptr_u64_i32_i32(fptr, a, b, c, d, e)",
       "Call fptr(ptr, ptr, ptr, u64, i32) and return i32.")
RT_DEF("__call6", rt_call6, 7, "fn __call6(fptr, a, b, c, d, e, f)", "Call fptr with 6 args.")
RT_DEF("__call7", rt_call7, 8, "fn __call7(fptr, ...)", "Call fptr with 7 args.")
RT_DEF("__call7_void", rt_call7_void, 8, "fn __call7_void(fptr, ...)",
       "Call fptr with seven arguments and no return value.")
RT_DEF("__call8", rt_call8, 9, "fn __call8(fptr, ...)", "Call fptr with 8 args.")
RT_DEF("__call9", rt_call9, 10, "fn __call9(fptr, ...)", "Call fptr with 9 args.")
RT_DEF("__call9_void", rt_call9_void, 10, "fn __call9_void(fptr, ...)",
       "Call fptr with nine arguments and no return value.")
RT_DEF("__call10", rt_call10, 11, "fn __call10(fptr, ...)", "Call fptr with 10 args.")
RT_DEF("__call11", rt_call11, 12, "fn __call11(fptr, ...)", "Call fptr with 11 args.")
RT_DEF("__call12", rt_call12, 13, "fn __call12(fptr, ...)", "Call fptr with 12 args.")
RT_DEF("__call13", rt_call13, 14, "fn __call13(fptr, ...)", "Call fptr with 13 args.")
RT_DEF("__call14", rt_call14, 15, "fn __call14(fptr, ...)", "Call fptr with 14 args.")
RT_DEF("__call15", rt_call15, 16, "fn __call15(fptr, ...)", "Call fptr with 15 args.")
RT_DEF("__set_args", rt_set_args, 3, "fn __set_args(ac, av, ep)",
       "Initialize command line arguments.")
RT_DEF("__parse_ast", rt_parse_ast, 1, "fn __parse_ast(s)", "Parses a string into an AST object.")
RT_DEF("__globals", rt_globals_get, 0, "fn __globals()",
       "Returns the pointer to the global variables table.")
RT_DEF("__set_globals", rt_globals_set, 1, "fn __set_globals(p)",
       "Sets the pointer to the global variables table.")
RT_DEF("__fix_fn_ptr", rt_fix_fn_ptr, 1, "fn __fix_fn_ptr(fn)",
       "Internal: canonicalize runtime function pointers for execution.")
RT_DEF("__panic", rt_panic, 1, "fn __panic(msg)", "Panics with a message.")
RT_DEF("__breakpoint", rt_breakpoint, 0, "fn __breakpoint()",
       "Triggers a debugger breakpoint trap on the current architecture.")
RT_DEF("__set_panic_env", rt_set_panic_env, 1, "fn __set_panic_env(e)",
       "Internal: sets panic jump environment.")
RT_DEF("__clear_panic_env", rt_clear_panic_env, 0, "fn __clear_panic_env()",
       "Internal: clears panic jump environment.")
RT_DEF("__jmpbuf_size", rt_jmpbuf_size, 0, "fn __jmpbuf_size()",
       "Internal: returns size of jmp_buf.")
RT_DEF("__jmpbuf_align", rt_jmpbuf_align, 0, "fn __jmpbuf_align()",
       "Internal: returns alignment of jmp_buf.")
RT_DEF("__get_panic_val", rt_get_panic_val, 0, "fn __get_panic_val()",
       "Internal: returns the panic message.")
RT_DEF("__index_read_probe_enabled", rt_index_read_probe_enabled, 0,
       "fn __index_read_probe_enabled()", "Internal: returns true when index parity probe logging is enabled.")
RT_DEF("__index_read_probe", rt_index_read_probe, 3, "fn __index_read_probe(tag, idx, path)",
       "Internal: debug parity hook for indexed reads (path: 0 slow, 1 fast).")
RT_DEF("__trace_loc", rt_trace_loc, 3, "fn __trace_loc(file, line, col)",
       "Internal: record the last executed source location.")
RT_DEF("__trace_func", rt_trace_func, 1, "fn __trace_func(name)",
       "Internal: record the current function name.")
RT_DEF("__trace_enter", rt_trace_enter, 3, "fn __trace_enter(func, file, line)",
       "Internal: push function onto call stack and record entry.")
RT_DEF("__trace_exit", rt_trace_exit, 0, "fn __trace_exit()",
       "Internal: pop function from call stack.")
RT_DEF("__trace_ret_void", rt_trace_ret_void, 0, "fn __trace_ret_void()",
       "Internal: trace a return without a printable value.")
RT_DEF("__trace_ret_tagged", rt_trace_ret_tagged, 1, "fn __trace_ret_tagged(v)",
       "Internal: trace a tagged return value.")
RT_DEF("__trace_ret_i64", rt_trace_ret_i64, 1, "fn __trace_ret_i64(v)",
       "Internal: trace a signed integer return value.")
RT_DEF("__trace_ret_u64", rt_trace_ret_u64, 1, "fn __trace_ret_u64(v)",
       "Internal: trace an unsigned integer return value.")
RT_DEF("__trace_ret_bool", rt_trace_ret_bool, 1, "fn __trace_ret_bool(v)",
       "Internal: trace a bool return value.")
RT_DEF("__trace_ret_ptr", rt_trace_ret_ptr, 1, "fn __trace_ret_ptr(v)",
       "Internal: trace a pointer return value.")
RT_DEF("__trace_ret_f64_bits", rt_trace_ret_f64_bits, 1, "fn __trace_ret_f64_bits(v)",
       "Internal: trace an f64 return value encoded as IEEE bits.")
RT_DEF("__trace_dump", rt_trace_dump, 1, "fn __trace_dump(n)",
       "Internal: dump recent trace entries.")
RT_DEF("__get_backtrace", rt_get_backtrace, 1, "fn __get_backtrace(n)",
       "Returns the current Nytrix backtrace as a list of [file, line, col, "
       "func] frames.")
RT_DEF("__push_defer", rt_push_defer, 2, "fn __push_defer(f, e)", "Internal: push defer.")
RT_DEF("__pop_run_defer", rt_pop_run_defer, 0, "fn __pop_run_defer()",
       "Internal: pop and run one defer.")
RT_DEF("__run_defers_to", rt_run_defers_to, 1, "fn __run_defers_to(n)", "Internal: run defers.")

RT_DEF("__argc", rt_argc, 0, "fn __argc()", "Returns the number of command-line arguments.")
RT_DEF("__argvp", rt_argvp, 0, "fn __argvp()", "Returns the raw argv pointer.")
RT_DEF("__argv", rt_argv, 1, "fn __argv(i)", "Returns the command-line argument string at index i.")
RT_DEF("__envc", rt_envc, 0, "fn __envc()", "Returns the number of environment variables.")
RT_DEF("__envp", rt_envp, 0, "fn __envp()", "Returns the raw environment variables pointer.")
RT_DEF("__errno", rt_errno, 0, "fn __errno()", "Returns the last error number.")
RT_DEF("__result_ok", rt_result_ok, 1, "fn __result_ok(v)", "Creates an Ok result.")
RT_DEF("__result_err", rt_result_err, 1, "fn __result_err(e)", "Creates an Err result.")
RT_DEF("__is_ok", rt_is_ok, 1, "fn __is_ok(v)", "Checks if value is an Ok result.")
RT_DEF("__is_err", rt_is_err, 1, "fn __is_err(v)", "Checks if value is an Err result.")
RT_DEF("__unwrap", rt_unwrap, 1, "fn __unwrap(v)", "Unwraps a Result or returns the value.")

RT_DEF("__thread_spawn", rt_thread_spawn, 2, "fn __thread_spawn(fn, arg)", "Spawns a new thread.")
RT_DEF("__thread_spawn_call", rt_thread_spawn_call, 3, "fn __thread_spawn_call(fn, argc, argv)",
       "Spawns a new thread and invokes fn with argc arguments from argv.")
RT_DEF("__thread_launch_call", rt_thread_launch_call, 3, "fn __thread_launch_call(fn, argc, argv)",
       "Launches a detached thread and invokes fn with argc arguments.")
RT_DEF("__thread_join", rt_thread_join, 1, "fn __thread_join(t)", "Joins a thread.")
RT_DEF("__mutex_new", rt_mutex_new, 0, "fn __mutex_new()", "Creates a new mutex.")
RT_DEF("__mutex_lock64", rt_mutex_lock64, 1, "fn __mutex_lock64(m)", "Locks a mutex.")
RT_DEF("__mutex_unlock64", rt_mutex_unlock64, 1, "fn __mutex_unlock64(m)", "Unlocks a mutex.")
RT_DEF("__mutex_free", rt_mutex_free, 1, "fn __mutex_free(m)", "Frees a mutex.")

RT_DEF("__to_str", rt_to_str, 1, "fn __to_str(v)", "Converts primitive to string.")
RT_DEF("__cstr_to_str", rt_cstr_to_str, 1, "fn __cstr_to_str(p)",
       "Copies a native NUL-terminated C string into a Nytrix string.")
RT_DEF("__str_concat", rt_str_concat, 2, "fn __str_concat(a, b)", "Concatenates two strings.")
RT_DEF("__str_builder_new", rt_str_builder_new, 1, "fn __str_builder_new(cap)",
       "Creates an internal string builder.")
RT_DEF("__str_builder_append", rt_str_builder_append, 2, "fn __str_builder_append(builder, value)",
       "Appends a value to an internal string builder.")
RT_DEF("__str_builder_to_str", rt_str_builder_to_str, 1, "fn __str_builder_to_str(builder)",
       "Finishes an internal string builder as a string.")
RT_DEF("__str_builder_free", rt_str_builder_free, 1, "fn __str_builder_free(builder)",
       "Frees an internal string builder.")
RT_DEF("__str_hash", rt_str_hash, 1, "fn __str_hash(s)", "Hashes a Nytrix string for dictionaries.")
RT_DEF("__str_eq", rt_str_eq, 2, "fn __str_eq(a, b)", "Compares two Nytrix strings byte-wise.")
RT_DEF("__proof_cert_digest", rt_proof_cert_digest, 4,
       "fn __proof_cert_digest(canonical, module_version, dependency_digest, checker_version)",
       "Computes the compact proof-certificate envelope digest.")
RT_DEF("__proof_cert_check", rt_proof_cert_check, 10,
       "fn __proof_cert_check(canonical, digest, module_version, dependency_digest, checker_version, max_variables, max_nodes, max_depth, max_steps, max_memory)",
       "Checks a canonical propositional certificate under explicit budgets.")
RT_DEF("__memcpy", rt_memcpy, 3, "fn __memcpy(d, s, n)", "Copies n bytes from s to d.")
RT_DEF("__memcmp", rt_memcmp, 3, "fn __memcmp(a, b, n)", "Compares n bytes of a and b.")
RT_DEF("__memset", rt_memset, 3, "fn __memset(p, v, n)", "Sets n bytes of p to v.")
RT_DEF("__rand64", rt_rand64, 0, "fn __rand64()", "Returns a random 64-bit integer.")
RT_DEF("__srand", rt_srand, 1, "fn __srand(s)", "Seeds the random number generator.")
RT_DEF("__copy_mem", rt_copy_mem, 3, "fn __copy_mem(d, s, n)",
       "Copies n bytes from s to d (llvm intrinsic).")
RT_DEF("__simd_mat4_mul", rt_simd_mat4_mul, 3, "fn __simd_mat4_mul(a, b, out)",
       "SIMD-accelerated 4x4 column-major float matrix multiply "
       "(SSE2/NEON/scalar).")
RT_DEF("__simd_mat4_mul_ptr", rt_simd_mat4_mul_ptr, 3,
       "fn __simd_mat4_mul_ptr(a_ptr, b_ptr, out_ptr)",
       "SIMD-accelerated 4x4 float matrix multiply using raw pointers.")
RT_DEF("__simmd_has_feature", rt_simmd_has_feature, 1, "fn __simmd_has_feature(name)",
       "Returns true when the host/runtime supports a named SIMD or instruction feature.")
RT_DEF("__simmd_popcnt64", rt_simmd_popcnt64, 1, "fn __simmd_popcnt64(x)",
       "Counts set bits in a 64-bit integer.")
RT_DEF("__simmd_ctz64", rt_simmd_ctz64, 1, "fn __simmd_ctz64(x)",
       "Counts trailing zero bits in a 64-bit integer; returns 64 for zero.")
RT_DEF("__simmd_clz64", rt_simmd_clz64, 1, "fn __simmd_clz64(x)",
       "Counts leading zero bits in a 64-bit integer; returns 64 for zero.")
RT_DEF("__simmd_bswap64", rt_simmd_bswap64, 1, "fn __simmd_bswap64(x)",
       "Byte-swaps a 64-bit integer.")
RT_DEF("__simmd_popcnt32", rt_simmd_popcnt32, 1, "fn __simmd_popcnt32(x)",
       "Counts set bits in a 32-bit integer.")
RT_DEF("__simmd_ctz32", rt_simmd_ctz32, 1, "fn __simmd_ctz32(x)",
       "Counts trailing zero bits in a 32-bit integer; returns 32 for zero.")
RT_DEF("__simmd_clz32", rt_simmd_clz32, 1, "fn __simmd_clz32(x)",
       "Counts leading zero bits in a 32-bit integer; returns 32 for zero.")
RT_DEF("__simmd_bswap32", rt_simmd_bswap32, 1, "fn __simmd_bswap32(x)",
       "Byte-swaps a 32-bit integer.")
RT_DEF("__simmd_rotl32", rt_simmd_rotl32, 2, "fn __simmd_rotl32(x, k)",
       "Rotates a 32-bit integer left by k bits.")
RT_DEF("__simmd_rotr32", rt_simmd_rotr32, 2, "fn __simmd_rotr32(x, k)",
       "Rotates a 32-bit integer right by k bits.")
RT_DEF("__simmd_rotl64", rt_simmd_rotl64, 2, "fn __simmd_rotl64(x, k)",
       "Rotates a 64-bit integer left by k bits.")
RT_DEF("__simmd_rotr64", rt_simmd_rotr64, 2, "fn __simmd_rotr64(x, k)",
       "Rotates a 64-bit integer right by k bits.")
RT_DEF("__simmd_prefetch", rt_simmd_prefetch, 3, "fn __simmd_prefetch(ptr, rw, locality)",
       "Issues a CPU prefetch hint for a raw pointer.")
RT_DEF("__simmd_pause", rt_simmd_pause, 0, "fn __simmd_pause()",
       "Issues a spin-wait pause/yield instruction where supported.")
RT_DEF("__simmd_lfence", rt_simmd_lfence, 0, "fn __simmd_lfence()",
       "Issues a load/acquire fence.")
RT_DEF("__simmd_sfence", rt_simmd_sfence, 0, "fn __simmd_sfence()",
       "Issues a store/release fence.")
RT_DEF("__simmd_mfence", rt_simmd_mfence, 0, "fn __simmd_mfence()",
       "Issues a full memory fence.")
RT_DEF("__simmd_rdtsc", rt_simmd_rdtsc, 0, "fn __simmd_rdtsc()",
       "Reads the x86 TSC when available; returns 0 on unsupported targets.")
RT_DEF("__simmd_crc32_u8", rt_simmd_crc32_u8, 2, "fn __simmd_crc32_u8(crc, byte)",
       "Updates a CRC32C accumulator with one byte, using SSE4.2 when available.")
RT_DEF("__simmd_crc32_u64", rt_simmd_crc32_u64, 2, "fn __simmd_crc32_u64(crc, word)",
       "Updates a CRC32C accumulator with one little-endian 64-bit word.")
RT_DEF("__simmd_pext64", rt_simmd_pext64, 2, "fn __simmd_pext64(x, mask)",
       "Parallel bit extract, using BMI2 when available.")
RT_DEF("__simmd_pdep64", rt_simmd_pdep64, 2, "fn __simmd_pdep64(x, mask)",
       "Parallel bit deposit, using BMI2 when available.")
RT_DEF("__simmd_clmul64_lo", rt_simmd_clmul64_lo, 2, "fn __simmd_clmul64_lo(x, y)",
       "Low 64 bits of carry-less 64x64 GF(2) multiplication.")
RT_DEF("__simmd_clmul64_hi", rt_simmd_clmul64_hi, 2, "fn __simmd_clmul64_hi(x, y)",
       "High 64 bits of carry-less 64x64 GF(2) multiplication.")
RT_DEF("__simmd_u8x16_xor_ptr", rt_simmd_u8x16_xor_ptr, 3,
       "fn __simmd_u8x16_xor_ptr(a, b, out)",
       "Unaligned 16-byte vector XOR from raw pointers.")
RT_DEF("__simmd_u8x16_and_ptr", rt_simmd_u8x16_and_ptr, 3,
       "fn __simmd_u8x16_and_ptr(a, b, out)",
       "Unaligned 16-byte vector AND from raw pointers.")
RT_DEF("__simmd_u8x16_or_ptr", rt_simmd_u8x16_or_ptr, 3,
       "fn __simmd_u8x16_or_ptr(a, b, out)",
       "Unaligned 16-byte vector OR from raw pointers.")
RT_DEF("__simmd_u8x16_add_ptr", rt_simmd_u8x16_add_ptr, 3,
       "fn __simmd_u8x16_add_ptr(a, b, out)",
       "Unaligned 16-byte vector wrapping add from raw pointers.")
RT_DEF("__simmd_u8x16_sub_ptr", rt_simmd_u8x16_sub_ptr, 3,
       "fn __simmd_u8x16_sub_ptr(a, b, out)",
       "Unaligned 16-byte vector wrapping subtract from raw pointers.")
RT_DEF("__simmd_u8x16_cmpeq_mask_ptr", rt_simmd_u8x16_cmpeq_mask_ptr, 2,
       "fn __simmd_u8x16_cmpeq_mask_ptr(a, b)",
       "Returns a 16-bit lane equality mask for two unaligned byte vectors.")
RT_DEF("__simmd_u8x16_shuffle_ptr", rt_simmd_u8x16_shuffle_ptr, 3,
       "fn __simmd_u8x16_shuffle_ptr(a, mask, out)",
       "Unaligned byte-lane shuffle with SSSE3/NEON/scalar semantics.")
RT_DEF("__simmd_u16x8_add_ptr", rt_simmd_u16x8_add_ptr, 3,
       "fn __simmd_u16x8_add_ptr(a, b, out)",
       "Unaligned 8-lane u16 vector wrapping add from raw pointers.")
RT_DEF("__simmd_u16x8_sub_ptr", rt_simmd_u16x8_sub_ptr, 3,
       "fn __simmd_u16x8_sub_ptr(a, b, out)",
       "Unaligned 8-lane u16 vector wrapping subtract from raw pointers.")
RT_DEF("__simmd_u16x8_mullo_ptr", rt_simmd_u16x8_mullo_ptr, 3,
       "fn __simmd_u16x8_mullo_ptr(a, b, out)",
       "Unaligned 8-lane u16 vector low-half multiply from raw pointers.")
RT_DEF("__simmd_i32x4_add_ptr", rt_simmd_i32x4_add_ptr, 3,
       "fn __simmd_i32x4_add_ptr(a, b, out)",
       "Unaligned 4-lane i32 vector add from raw pointers.")
RT_DEF("__simmd_i32x4_sub_ptr", rt_simmd_i32x4_sub_ptr, 3,
       "fn __simmd_i32x4_sub_ptr(a, b, out)",
       "Unaligned 4-lane i32 vector subtract from raw pointers.")
RT_DEF("__simmd_i32x4_mullo_ptr", rt_simmd_i32x4_mullo_ptr, 3,
       "fn __simmd_i32x4_mullo_ptr(a, b, out)",
       "Unaligned 4-lane i32 vector low-half multiply from raw pointers.")
RT_DEF("__simmd_i32x4_xor_ptr", rt_simmd_i32x4_xor_ptr, 3,
       "fn __simmd_i32x4_xor_ptr(a, b, out)",
       "Unaligned 4-lane i32 vector XOR from raw pointers.")
RT_DEF("__simmd_u32x4_and_ptr", rt_simmd_u32x4_and_ptr, 3,
       "fn __simmd_u32x4_and_ptr(a, b, out)",
       "Unaligned 4-lane u32 vector AND from raw pointers.")
RT_DEF("__simmd_u32x4_or_ptr", rt_simmd_u32x4_or_ptr, 3,
       "fn __simmd_u32x4_or_ptr(a, b, out)",
       "Unaligned 4-lane u32 vector OR from raw pointers.")
RT_DEF("__simmd_u64x2_add_ptr", rt_simmd_u64x2_add_ptr, 3,
       "fn __simmd_u64x2_add_ptr(a, b, out)",
       "Unaligned 2-lane u64 vector wrapping add from raw pointers.")
RT_DEF("__simmd_u64x2_xor_ptr", rt_simmd_u64x2_xor_ptr, 3,
       "fn __simmd_u64x2_xor_ptr(a, b, out)",
       "Unaligned 2-lane u64 vector XOR from raw pointers.")
RT_DEF("__simmd_u64x2_and_ptr", rt_simmd_u64x2_and_ptr, 3,
       "fn __simmd_u64x2_and_ptr(a, b, out)",
       "Unaligned 2-lane u64 vector AND from raw pointers.")
RT_DEF("__simmd_u64x2_or_ptr", rt_simmd_u64x2_or_ptr, 3,
       "fn __simmd_u64x2_or_ptr(a, b, out)",
       "Unaligned 2-lane u64 vector OR from raw pointers.")
RT_DEF("__simmd_f32x4_add_ptr", rt_simmd_f32x4_add_ptr, 3,
       "fn __simmd_f32x4_add_ptr(a, b, out)",
       "Unaligned 4-lane f32 vector add from raw pointers.")
RT_DEF("__simmd_f32x4_sub_ptr", rt_simmd_f32x4_sub_ptr, 3,
       "fn __simmd_f32x4_sub_ptr(a, b, out)",
       "Unaligned 4-lane f32 vector subtract from raw pointers.")
RT_DEF("__simmd_f32x4_mul_ptr", rt_simmd_f32x4_mul_ptr, 3,
       "fn __simmd_f32x4_mul_ptr(a, b, out)",
       "Unaligned 4-lane f32 vector multiply from raw pointers.")
RT_DEF("__simmd_f32x4_div_ptr", rt_simmd_f32x4_div_ptr, 3,
       "fn __simmd_f32x4_div_ptr(a, b, out)",
       "Unaligned 4-lane f32 vector divide from raw pointers.")
RT_DEF("__simmd_f32x4_min_ptr", rt_simmd_f32x4_min_ptr, 3,
       "fn __simmd_f32x4_min_ptr(a, b, out)",
       "Unaligned 4-lane f32 vector min from raw pointers.")
RT_DEF("__simmd_f32x4_max_ptr", rt_simmd_f32x4_max_ptr, 3,
       "fn __simmd_f32x4_max_ptr(a, b, out)",
       "Unaligned 4-lane f32 vector max from raw pointers.")
RT_DEF("__simmd_f32x4_sqrt_ptr", rt_simmd_f32x4_sqrt_ptr, 2,
       "fn __simmd_f32x4_sqrt_ptr(a, out)",
       "Unaligned 4-lane f32 vector square root from raw pointers.")
RT_DEF("__simmd_f32x4_fma_ptr", rt_simmd_f32x4_fma_ptr, 4,
       "fn __simmd_f32x4_fma_ptr(a, b, c, out)",
       "Unaligned 4-lane f32 fused multiply-add from raw pointers where supported.")
RT_DEF("__simmd_f64x2_add_ptr", rt_simmd_f64x2_add_ptr, 3,
       "fn __simmd_f64x2_add_ptr(a, b, out)",
       "Unaligned 2-lane f64 vector add from raw pointers.")
RT_DEF("__simmd_f64x2_sub_ptr", rt_simmd_f64x2_sub_ptr, 3,
       "fn __simmd_f64x2_sub_ptr(a, b, out)",
       "Unaligned 2-lane f64 vector subtract from raw pointers.")
RT_DEF("__simmd_f64x2_mul_ptr", rt_simmd_f64x2_mul_ptr, 3,
       "fn __simmd_f64x2_mul_ptr(a, b, out)",
       "Unaligned 2-lane f64 vector multiply from raw pointers.")
RT_DEF("__simmd_f64x2_div_ptr", rt_simmd_f64x2_div_ptr, 3,
       "fn __simmd_f64x2_div_ptr(a, b, out)",
       "Unaligned 2-lane f64 vector divide from raw pointers.")
RT_DEF("__simmd_f64x2_sqrt_ptr", rt_simmd_f64x2_sqrt_ptr, 2,
       "fn __simmd_f64x2_sqrt_ptr(a, out)",
       "Unaligned 2-lane f64 vector square root from raw pointers.")
RT_DEF("__simmd_f64x2_fma_ptr", rt_simmd_f64x2_fma_ptr, 4,
       "fn __simmd_f64x2_fma_ptr(a, b, c, out)",
       "Unaligned 2-lane f64 fused multiply-add from raw pointers where supported.")
RT_DEF("__simmd_byte_class_reduce", rt_simmd_byte_class_reduce, 7,
       "fn __simmd_byte_class_reduce(ptr, len, rounds, class_lo, class_hi, hit, miss)",
       "Counts/reduces bytes against a 128-bit ASCII class mask, using AVX2/SSE2/NEON when available.")
RT_DEF("__simmd_jsonscan_ascii", rt_simmd_jsonscan_ascii, 3,
       "fn __simmd_jsonscan_ascii(ptr, len, rounds)",
       "Specialized JSON ASCII scan checksum kernel.")
RT_DEF("__simmd_i32_hash_put_ptr", rt_simmd_i32_hash_put_ptr, 6,
       "fn __simmd_i32_hash_put_ptr(keys, values, used, cap, key, value)",
       "Inserts one i32 key/value into a raw power-of-two linear-probe table.")
RT_DEF("__simmd_i32_hash_probe_sum_ptr", rt_simmd_i32_hash_probe_sum_ptr, 8,
       "fn __simmd_i32_hash_probe_sum_ptr(keys, values, used, cap, probe_keys, probe_weights, n, rounds)",
       "Probes a raw i32 linear-probe table and computes a join checksum.")
RT_DEF("__simmd_i32_sqlscan_sum_ptr", rt_simmd_i32_sqlscan_sum_ptr, 6,
       "fn __simmd_i32_sqlscan_sum_ptr(region, tier, amount, flags, n, rounds)",
       "Runs a raw i32 column filter/aggregate checksum kernel.")
RT_DEF("__mat4_to_buffer", rt_mat4_to_buffer, 2, "fn __mat4_to_buffer(m, buf)",
       "Optimized conversion of 4x4 matrix object to raw float buffer.")
RT_DEF("__mat4_from_buffer", rt_mat4_from_buffer, 2, "fn __mat4_from_buffer(m, buf)",
       "Optimized conversion of raw float buffer back into 4x4 matrix object.")
RT_DEF("__os_name", rt_os_name, 0, "fn __os_name()", "Returns the name of the operating system.")
RT_DEF("__arch_name", rt_arch_name, 0, "fn __arch_name()", "Returns the name of the architecture.")
RT_DEF("__main", rt_main, 0, "fn __main()", "Checks if the current module is the main entry point.")

#ifndef RT_GV
#define RT_GV(n, p, t, d)
#endif

RT_GV("std.core.primitives.__argc_val", rt_argc_val, int64_t, "Global: argc value.")
RT_GV("std.core.primitives.__envc_val", rt_envc_val, int64_t, "Global: envc value.")
RT_GV("std.core.primitives.__argv_ptr", rt_argv_ptr, int64_t *, "Global: argv pointer.")
RT_GV("std.core.primitives.__envp_ptr", rt_envp_ptr, int64_t *, "Global: envp pointer.")
RT_GV("std.core.primitives.__errno_val", rt_errno_val, int64_t, "Global: errno value.")
RT_GV("std.core.primitives.__globals_ptr", rt_globals_ptr, int64_t,
      "Global: globals table pointer.")

RT_DEF("__print_int", rt_print_int, 1, "fn __print_int(v)", "Fast integer print.")
RT_DEF("__print_newline", rt_print_newline, 0, "fn __print_newline()", "Fast newline.")
RT_DEF("__print_str_raw", rt_print_str_raw, 1, "fn __print_str_raw(s)", "Fast string print.")
#undef RT_GV
