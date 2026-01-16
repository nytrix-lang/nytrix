;; Keywords: io mod
;; Io Mod module.

use std.core
use std.strings.str
module std.io (sys_write, sys_read, sys_open, sys_close, sys_stat, sys_fstat,
   write_fd, read_fd, _print_write, print, input, file_read, file_write, file_append,
   file_exists, file_remove, cwd, __repl_show, _print_marker)

fn sys_write(fd, buf, n, i=0){
  "Writes `n` bytes from buffer `buf + i` to file descriptor `fd`. Wraps the **write(2)** system call."
  if(i == 0){ __syscall(1, fd, buf, n, 0,0,0) }
  else { __sys_write_off(fd, buf, n, i) }
}

fn sys_read(fd, buf, n, i=0){
  "Reads up to `n` bytes from file descriptor `fd` into buffer `buf + i`. Wraps the **read(2)** system call."
  if(i == 0){ __syscall(0, fd, buf, n, 0,0,0) }
  else { __sys_read_off(fd, buf, n, i) }
}

fn sys_open(path, flags, mode){
  "Raw **open(2)** system call. Opens the file at `path` string with specific `flags` and `mode` bits."
  __syscall(2, path, flags, mode, 0, 0, 0)
}

fn sys_close(fd){
  "Raw **close(2)** system call. Closes the file descriptor `fd`."
  __syscall(3, fd, 0, 0, 0, 0, 0)
}

fn sys_stat(path, buf){
  "Raw **stat(2)** system call. Populates `buf` with file metadata for `path`."
  __syscall(4, path, buf, 0, 0, 0, 0)
}

fn sys_fstat(fd, buf){
  "Raw **fstat(2)** system call. Populates `buf` with file metadata for open descriptor `fd`."
  __syscall(5, fd, buf, 0, 0, 0, 0)
}



fn write_fd(fd, buf, len){
   "Writes `len` bytes from any object `buf` to file descriptor `fd`."
   sys_write(fd, buf, len)
}

fn read_fd(fd, buf, n){
   "Reads up to `n` bytes from descriptor `fd` into buffer `buf`."
   sys_read(fd, buf, n)
}

fn _print_write(s){
  "Internal: writes string `s` directly to stdout (fd 1)."
  sys_write(1, s, str_len(s))
  0
}

fn print(...args){
  "Prints objects to stdout with formatting.
  - **sep**: String inserted between values (default: space).
  - **end**: String printed at the end (default: newline).
  - Supports multiple arguments and raw Nytrix objects."
  def num_args = list_len(args)
  if(num_args == 0){
    sys_write(1, "\n", 1)
    0
  } else {
    def end = "\n"
    def step = " "
    def i = 0
    ; Scan for kwargs
    while(i < num_args){
       def item = get(args, i)
       if(is_kwargs(item)){
          def k = get_kwarg_key(item)
          def v = get_kwarg_val(item)
          if(k == "end"){ end = v }
          if(k == "step" || k == "sep"){ step = v }
       }
       i += 1
    }
    ; Print non-kwargs values
    i = 0
    def printed = 0
    def first = true
    while(i < num_args){
       def item = get(args, i)
       if(is_kwargs(item) == false){
          if(first == false){
             sys_write(1, step, str_len(step))
          }
          def s = to_str(item)
          sys_write(1, s, str_len(s))
          first = false
       }
       i += 1
    }
    sys_write(1, end, str_len(end))
    0
  }
}

fn input(prompt){
  "Displays the `prompt` string and waits for a line from stdin. Returns the line as a string (trimmed of newline)."
  if(prompt != 0){
     sys_write(1, prompt, str_len(prompt))
  }
  def cap = 8192
  def buf = __malloc(cap)
  store64(buf, 120, -8) ; String tag
  def pos = 0
  while(1){
     def n = sys_read(0, buf, cap - pos - 1)
     if(n <= 0){ break }
     def i = 0
     while(i < n){
        if(load8(buf, pos + i) == 10){ ; Newline character
           pos += i
           store8(buf, 0, pos)
           store64(buf, pos, -16)
           return buf
        }
        i += 1
     }
     pos += n
     if(pos + 1 >= cap){
        def newcap = cap * 2
        def new_buf = __malloc(newcap)
        store64(new_buf, 120, -8)
        __memcpy(new_buf, buf, pos)
        __free(buf)
        buf = new_buf
        cap = newcap
     }
  }
  store8(buf, 0, pos)
  store64(buf, pos, -16)
  buf
}

fn file_read(path){
  "Reads the **entire** content of the file at `path` and returns it as a string. Returns empty string on failure."
  def fd = sys_open(path, 0, 0) ; O_RDONLY
  if(fd < 0){ "" }
  else {
    def cap = 8192
    def buf = __malloc(cap)
    store64(buf, 120, -8)
    def pos = 0
    while(1){
       def n = sys_read(fd, buf, cap - pos - 1, pos)
       if(n <= 0){ break }
       pos += n
       if(pos + 1 >= cap){
          def next_cap = cap * 2
          def next_buf = __malloc(next_cap)
          store64(next_buf, 120, -8)
          __memcpy(next_buf, buf, pos)
          __free(buf)
          buf = next_buf
          cap = next_cap
       }
    }
    sys_close(fd)
    store8(buf, 0, pos) ; Null-terminate
    store64(buf, pos, -16)
    buf
  }
}

fn file_write(path, data){
  "Overwrites the file at `path` with string `data`. Creates the file if it doesn't exist."
  def fd = sys_open(path, 577, 420) ; O_WRONLY | O_CREAT | O_TRUNC
  if(fd < 0){ -1 }
  else {
    def n = sys_write(fd, data, str_len(data))
    sys_close(fd)
    n
  }
}

fn file_append(path, data){
  "Appends string `data` to the file at `path`. Creates the file if it doesn't exist."
  def fd = sys_open(path, 1089, 420) ; O_WRONLY | O_CREAT | O_APPEND
  if(fd < 0){ -1 }
  else {
    def n = sys_write(fd, data, str_len(data))
    sys_close(fd)
    n
  }
}

fn file_exists(path){
  "Returns **true** if a file or directory exists at `path`."
  def buf = __malloc(144)
  def r = sys_stat(path, buf)
  __free(buf)
  r == 0
}

fn file_remove(path){
  "Deletes the file at `path` using the **unlink(2)** system call."
  __syscall(87, path, 0, 0, 0, 0, 0) ; unlink
}

fn cwd(){
  "Returns the **current working directory** as a string using **getcwd(2)**."
  def buf = __malloc(4096)
  __store64_idx(buf, -8, 120)
  def n = __syscall(79, buf, 4096, 0, 0, 0, 0) ; getcwd
  if(n < 0){ __free(buf)  "" }
  else {
    __store8_idx(buf, n, 0)
    buf
  }
}

fn __repl_show(val){
  "Internal: prints `val` to stdout followed by a newline. Used by the interactive REPL."
  def s = __to_str(val)
  sys_write(1, s, str_len(s))
  sys_write(1, "\n", 1)
  val
}