;; Keywords: os process
;; Process module.

use std.core *
use std.os.sys *
use std.str *
use std.core.reflect *

module std.os.process (
   run, popen, waitpid
)

fn waitpid(pid, options){
   "Waits for `pid` using `wait4(2)` and returns the raw wait status; returns a negative errno-style value on syscall failure."
   def status_ptr = malloc(8)
   def res = syscall(61, pid, status_ptr, options, 0, 0, 0) ;; wait4
   if(res < 0){
       free(status_ptr)
       return res
   }
   def status = load32(status_ptr, 0)
   free(status_ptr)
   status
}

fn run(path, args) {
    "Forks and execs `path` with `args`, waits for completion, and returns the child exit code (0..255); returns `-1` if fork fails."
    mut pid = syscall(57, 0, 0, 0, 0, 0, 0) ; fork()
    if (pid == 0) {
        def n = list_len(args)
        def argv = malloc((n + 1) * 8)
        mut i = 0
        while (i < n) {
            def s = get(args, i, 0)
            store64(argv, s, i * 8)
            i = i + 1
        }
        store64(argv, to_int(0), n * 8)
        __execve(path, argv, 0)
        syscall(60, 127, 0, 0, 0, 0, 0) ; exit(127) if exec fails
        return 1
    } else {
        if (pid < 0) { return -1 }
        def status_ptr = malloc(8)
        def wr = syscall(61, pid, status_ptr, 0, 0, 0, 0)
        if(wr < 0){
            free(status_ptr)
            return -1
        }
        def status = load32(status_ptr, 0)
        free(status_ptr)
        return (status / 256) % 256
    }
}

fn popen(path, args){
    "Starts `path` with stdin/stdout pipes and returns `[pid, child_stdin_fd, child_stdout_fd]`; returns `0` on setup failure."
    
    def p_stdin = malloc(8) ;; 2 ints = 8 bytes
    def p_stdout = malloc(8)
    
    if(syscall(22, p_stdin, 0,0,0,0,0) < 0){
        free(p_stdin)
        free(p_stdout)
        return 0
    } ;; pipe (syscall 22 for x86-64)
    if(syscall(22, p_stdout, 0,0,0,0,0) < 0){
        def in_r = load32(p_stdin, 0)
        def in_w = load32(p_stdin, 4)
        syscall(3, in_r, 0,0,0,0,0)
        syscall(3, in_w, 0,0,0,0,0)
        free(p_stdin)
        free(p_stdout)
        return 0
    }
    
    def stdin_read = load32(p_stdin, 0)
    def stdin_write = load32(p_stdin, 4)
    def stdout_read = load32(p_stdout, 0)
    def stdout_write = load32(p_stdout, 4)
    
    free(p_stdin)
    free(p_stdout)
    
    mut pid = syscall(57, 0, 0, 0, 0, 0, 0) ;; fork
    if(pid < 0){
        syscall(3, stdin_read, 0,0,0,0,0)
        syscall(3, stdin_write, 0,0,0,0,0)
        syscall(3, stdout_read, 0,0,0,0,0)
        syscall(3, stdout_write, 0,0,0,0,0)
        return 0
    }
    if(pid == 0){
        ;; Child
        ;; dup2(stdin_read, 0)
        syscall(33, stdin_read, 0, 0, 0, 0, 0)
        ;; dup2(stdout_write, 1)
        syscall(33, stdout_write, 1, 0, 0, 0, 0)
        
        ;; Close unused
        syscall(3, stdin_write, 0,0,0,0,0)
        syscall(3, stdout_read, 0,0,0,0,0)
        syscall(3, stdin_read, 0,0,0,0,0)
        syscall(3, stdout_write, 0,0,0,0,0)
        
        ;; Exec
        def n = list_len(args)
        def argv = malloc((n + 1) * 8)
        mut i = 0
        while(i < n){
            store64(argv, get(args, i, 0), i * 8)
            i = i + 1
        }
        store64(argv, to_int(0), n * 8)
        
        __execve(path, argv, 0)
        syscall(60, 127, 0, 0, 0, 0, 0)
        0
    } else {
        ;; Parent
        ;; Close child side
        syscall(3, stdin_read, 0,0,0,0,0)
        syscall(3, stdout_write, 0,0,0,0,0)
        
        mut ret = list(3)
        ret = append(ret, pid)
        ret = append(ret, stdin_write)
        ret = append(ret, stdout_read)
        ret
    }
}
