;;; mod.ny --- io mod module

;; Keywords: io mod

;;; Commentary:

;; Io Mod module.

use std.core
use std.strings.str
module std.io (
	sys_write, sys_read, sys_open, sys_close, sys_stat, sys_fstat, file_open, file_close,
	write_fd, read_fd, _print_write, print, input, file_read, file_write, file_append,
	file_exists, file_remove, cwd, __repl_show, _print_marker
)

; define IO_BUF = 8192

fn sys_write(fd, buf, n, i=0){
  "Writes `n` bytes from buffer `buf + i` to file descriptor `fd`."
  if(i == 0){ return rt_syscall(1, fd, buf, n, 0,0,0) }
  return rt_sys_write_off(fd, buf, n, i)
}

fn sys_read(fd, buf, n, i=0){
  "Reads `n` bytes from file descriptor `fd` into buffer `buf + i`."
  if(i == 0){ return rt_syscall(0, fd, buf, n, 0,0,0) }
  return rt_sys_read_off(fd, buf, n, i)
}

fn sys_open(path, flags, mode){
  "Opens the file at `path` with specified `flags` and `mode`. System call."
  return rt_syscall(2, path, flags, mode, 0, 0, 0)
}

fn sys_close(fd){
  "Closes the specified file descriptor. System call."
  return rt_syscall(3, fd, 0, 0, 0, 0, 0)
}

fn sys_stat(path, buf){
  "Retrieves file status for `path` into `buf`. System call."
  return rt_syscall(4, path, buf, 0, 0, 0, 0)
}

fn sys_fstat(fd, buf){
  "Retrieves file status for open file descriptor `fd` into `buf`. System call."
  return rt_syscall(5, fd, buf, 0, 0, 0, 0)
}

fn file_open(path, flags, mode){
  "Opens a file and returns its file descriptor."
  return sys_open(path, flags, mode)
}

fn file_close(fd){
  "Closes an open file descriptor."
  return sys_close(fd)
}

fn write_fd(fd, buf, len){
	"Write `len` bytes from `buf` to file descriptor `fd`."
	return sys_write(fd, buf, len)
}

fn read_fd(fd, buf, n){
	"Read up to `n` bytes from file descriptor `fd` into `buf`."
	return sys_read(fd, buf, n)
}

fn _print_write(s){
  "Internal: write a string to stdout without a newline."
  sys_write(1, s, str_len(s))
  return 0
}

fn print(...args){
	"Print values with separators and end; supports kwargs end/sep/step."
  def num_args = list_len(args)
  if(num_args == 0){
	 sys_write(1, "\n", 1)
	 return 0
  }
  def end = "\n"
  def step = " "
  def i = 0
  ; Scan for kwargs
  while(i < num_args){
	  def item = get(args, i)
	  if(is_kwarg(item)){
		  def k = get_kwarg_key(item)
		  def v = get_kwarg_val(item)
		  if(k == "end"){ end = v }
		  if(k == "step" || k == "sep"){ step = v }
	  }
	  i = i + 1
  }
  ; Print non-kwargs values
  i = 0
  def printed = 0
  def first = true
  while(i < num_args){
	  def item = get(args, i)
	  if(is_kwarg(item) == false){
		  if(first == false){
			  sys_write(1, step, str_len(step))
		  }
		  def s = _to_string(item)
		  sys_write(1, s, str_len(s))
		  first = false
	  }
	  i = i + 1
  }
  sys_write(1, end, str_len(end))
  return 0
}

fn input(prompt){
  "Displays `prompt` and reads a line of input from stdin. Returns the input as a string."
  if(prompt != 0){
	  sys_write(1, prompt, str_len(prompt))
  }
  def cap = 8192
  def buf = rt_malloc(cap)
  store64(buf, 120, -8) ; String tag
  def pos = 0
  while(1){
	  def n = sys_read(0, buf, cap - pos - 1)
	  if(n <= 0){ break }
	  def i = 0
	  while(i < n){
		  if(load8(buf, pos + i) == 10){ ; Newline character
			  pos = pos + i
			  store8(buf, 0, pos)
			  store64(buf, pos, -16)
			  return buf
		  }
		  i = i + 1
	  }
	  pos = pos + n
	  if(pos + 1 >= cap){
		  def newcap = cap * 2
		  def new_buf = rt_malloc(newcap)
		  store64(new_buf, 120, -8)
		  rt_memcpy(new_buf, buf, pos)
		  rt_free(buf)
		  buf = new_buf
		  cap = newcap
	  }
  }
  store8(buf, 0, pos)
  store64(buf, pos, -16)
  return buf
}

fn file_read(path){
  "Reads the entire content of a file into a string."
  def fd = sys_open(path, 0, 0) ; O_RDONLY
  if(fd < 0){ return "" }
  def cap = 8192
  def buf = rt_malloc(cap)
  store64(buf, 120, -8)
  def pos = 0
  while(1){
	  def n = sys_read(fd, buf, cap - pos - 1, pos)
	  if(n <= 0){ break }
	  pos = pos + n
	  if(pos + 1 >= cap){
		  def next_cap = cap * 2
		  def next_buf = rt_malloc(next_cap)
		  store64(next_buf, 120, -8)
		  rt_memcpy(next_buf, buf, pos)
		  rt_free(buf)
		  buf = next_buf
		  cap = next_cap
	  }
  }
  sys_close(fd)
  store8(buf, 0, pos) ; Null-terminate
  store64(buf, pos, -16)
  return buf
}

fn file_write(path, data){
  "Writes string `data` to the file at `path`, overwriting any existing content."
  def fd = sys_open(path, 577, 420) ; O_WRONLY | O_CREAT | O_TRUNC
  if(fd < 0){ return -1 }
  def n = sys_write(fd, data, str_len(data))
  sys_close(fd)
  return n
}

fn file_append(path, data){
  "Appends string `data` to the end of the file at `path`."
  def fd = sys_open(path, 1089, 420) ; O_WRONLY | O_CREAT | O_APPEND
  if(fd < 0){ return -1 }
  def n = sys_write(fd, data, str_len(data))
  sys_close(fd)
  return n
}

fn file_exists(path){
  "Check if a file or directory exists at the specified path."
  def buf = rt_malloc(144)
  def r = sys_stat(path, buf)
  rt_free(buf)
  return r == 0
}

fn file_remove(path){
  "Deletes the file at the specified path. System call."
  return rt_syscall(87, path, 0, 0, 0, 0, 0) ; unlink
}

fn cwd(){
  "Return the current working directory as a string."
  def buf = rt_malloc(4096)
  rt_store64_idx(buf, -8, 120)
  def n = rt_syscall(79, buf, 4096, 0, 0, 0, 0) ; getcwd
  if(n < 0){ rt_free(buf)  return "" }
  rt_store8_idx(buf, n, 0)
  return buf
}

fn __repl_show(val){
  "Internal: REPL value printer."
  def s = rt_to_str(val)
  sys_write(1, s, str_len(s))
  sys_write(1, "\n", 1)
  return val
}
