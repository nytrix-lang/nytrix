;;; mod.ny --- process mod module

;; Author: x3ric
;; Maintainer: x3ric
;; Keywords: process mod

;;; Commentary:

;; Process Mod module.

use std.collections
use std.os.sys
module std.process (
	fork, waitpid, pack_argv, execve, spawn, sys_pipe, dup2, kill, exec, run, popen, exit
)

fn fork(){
	"Forks the current process. Returns the child PID to the parent, and 0 to the child."
	return rt_syscall(57, 0,0,0,0,0,0)
}

fn waitpid(pid, options){
	"Waits for process `pid`. Returns list [pid, status] or -1 on error."
	def st = rt_malloc(8)
	def r = rt_syscall(61, pid, st, options, 0,0,0)
	if(r < 0){ rt_free(st) return -1 }
	def res = [r, load32(st)]
}

fn pack_argv(args){
	"Packs a list of strings into a C-style argv array (null-terminated)."
	def n = list_len(args)
	def arr = rt_malloc((n + 1) * 8)
	def i = 0
	while(i < n){
		store64(arr + i*8, get(args, i))
		i = i + 1
	}
	store64(arr + n*8, rt_to_int(0))
	return arr
}

fn execve(path, args){
	"Executes the program at `path` with arguments `args`. Replaces the current process image."
	def argvp = pack_argv(args)
	def envp = rt_envp()
	envp = rt_envp()
	return rt_syscall(59, path, argvp, envp, 0,0,0)
}
fn spawn(path, args){
	"Spawns a new process executing `path` with `args`. Returns the PID of the new process."
	def pid = fork()
									if(pid==0){
										execve(path, args)
										rt_exit(1)
									}    return pid
}

fn sys_pipe(){
	"Create a unidirectional data channel (pipe). Returns a list `[read_fd, write_fd]`."
	def fds = rt_malloc(16)
	def r = rt_syscall(22, fds, 0,0,0,0,0)
	if(r < 0){ return [0,0]  }
	return [load32(fds), load32(fds+4)]
}

fn dup2(oldfd, newfd){
	"Duplicates a file descriptor `oldfd` to `newfd`."
	return rt_syscall(33, oldfd, newfd, 0,0,0,0)
}

fn kill(pid, sig){
	"Sends signal `sig` to process `pid`."
	return rt_syscall(62, pid, sig, 0,0,0,0)
}

fn exec(path, args){
	"Convenience wrapper for `execve`. Prepares arguments and executes the program."
	def full_args = list(8)
	full_args = append(full_args, path)
	def i = 0
	while(i < list_len(args)){
		full_args = append(full_args, get(args, i))
		i = i + 1
	}
	return execve(path, full_args)
}

fn run(path, args){
	"Runs a command synchronously. Spawns the process and waits for it to finish. Returns the exit status."
	def pid = spawn(path, args)
	if(pid < 0){ return -1 }
	def status = waitpid(pid, 0)
	if(status == -1){ return -1 }
	return (get(status, 1) >> 8) & 255
}

fn popen(path, args){
	"Spawns a process with piped stdin/stdout. Returns `[pid, stdin_write_fd, stdout_read_fd]`."
	def in_pipe = sys_pipe()
	def out_pipe = sys_pipe()
	def pid = fork()
	if(pid == 0){
		; Child
		dup2(get(in_pipe, 0), 0)
		dup2(get(out_pipe, 1), 1)
		dup2(get(out_pipe, 1), 2)
		; Close all pipe fds in child after dup2
		rt_syscall(3, get(in_pipe, 0), 0,0,0,0,0)
		rt_syscall(3, get(in_pipe, 1), 0,0,0,0,0)
		rt_syscall(3, get(out_pipe, 0), 0,0,0,0,0)
		rt_syscall(3, get(out_pipe, 1), 0,0,0,0,0)
		def argvp = pack_argv(args)
		def s0 = load64(argvp)
		rt_syscall(1, 2, f"DEBUG Child: Current working directory: {getcwd()}\n", 0,0,0,0)
		def child_envp = rt_envp()
		rt_syscall(1, 2, f"DEBUG Child: envp pointer: {child_envp}\n", 0,0,0,0)
		def k = 0
		while(load64(child_envp + k * 8) != 0){
			rt_syscall(1, 2, f"DEBUG Child: envp[{k}]={load64(child_envp + k * 8)}\n", 0,0,0,0)
			k = k + 1
		}
		rt_syscall(59, path, argvp, child_envp, 0,0,0)
		rt_syscall(1, 2, "EXEC FAILED\n", 12, 0,0,0)
		rt_exit(1)    }
	; Parent: close child ends
	rt_syscall(3, get(in_pipe, 0), 0,0,0,0,0)
	rt_syscall(3, get(out_pipe, 1), 0,0,0,0,0)
	return [pid, get(in_pipe, 1), get(out_pipe, 0)]
}

fn exit(code){
	"Terminates the current process with the given status code."
	rt_exit(code)
}
