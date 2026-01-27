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
   "Waits for process `pid` to change state."
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
    "Runs a process and waits for it to finish. Returns exit code."
    mut pid = syscall(57, 0, 0, 0, 0, 0, 0) ; fork()
    if (pid == 0) {
        def n = list_len(args)
        def argv = malloc((n + 1) * 8)
        mut i = 0
        while (i < n) {
            def s = get(args, i)
            store64(argv + (i * 8), s)
            i = i + 1
        }
        store64(argv + (n * 8), 0)
        syscall(59, path, argv, envp(), 0, 0, 0)
        syscall(60, 1, 0, 0, 0, 0, 0) ; exit(1)
        return 1
    } else {
        if (pid < 0) { return -1 }
        def status_ptr = malloc(8)
        syscall(61, pid, status_ptr, 0, 0, 0, 0)
        def status = load64(status_ptr, 0)
        free(status_ptr)
        return (status / 256) % 256
    }
}

fn popen(path, args){
    "Starts a process with pipes for stdin, stdout, stderr. Returns [pid, stdin, stdout, stderr]."
    
    def p_stdin = malloc(8) ;; 2 ints = 8 bytes
    def p_stdout = malloc(8)
    
    if(syscall(22, p_stdin, 0,0,0,0,0) < 0){ return 0 } ;; pipe (syscall 22 for x86-64)
    if(syscall(22, p_stdout, 0,0,0,0,0) < 0){ return 0 }
    
    def stdin_read = load32(p_stdin, 0)
    def stdin_write = load32(p_stdin, 4)
    def stdout_read = load32(p_stdout, 0)
    def stdout_write = load32(p_stdout, 4)
    
    free(p_stdin)
    free(p_stdout)
    
    mut pid = syscall(57, 0, 0, 0, 0, 0, 0) ;; fork
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
        def n = len(args)
        def argv = malloc((n + 1) * 8)
        mut i = 0
        while(i < n){
            store64(argv, get(args, i), i * 8)
            i = i + 1
        }
        store64(argv, to_int(0), n * 8)
        
        syscall(59, path, argv, envp(), 0, 0, 0)
        syscall(60, 1, 0, 0, 0, 0, 0)
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

