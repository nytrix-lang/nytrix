;; Keywords: interact prompt interactive tube process socket ssh os
;; Compatibility facade for the buffered tube implementation in std.os.net.remote.
;; References:
;; - std.os
module std.os.interact(
   proc, process, shell, ssh, ssh_process, ssh_shell,
   dial, remote, connect, connect_retry,
   context, set_context, verbose, set_verbose, set_level, set_log_level, log_level, set_timeout,
   interactive, send, sendline, recv, recvn, recv_line, recvline,
   recv_all, recvall, recv_until, recvuntil, expect, expect_map,
   send_after, sendafter, sendline_after, sendlineafter,
   clean, buffered, unrecv, shutdown_send, close, connected, tube_kind,
   fileno, pid, transcript, transcript_text
)

use std.os.net.remote as tube

fn proc(str path, list args=[], any level="", int timeout_ms=-1, int chunk_size=0) any {
   "Starts a local process with piped stdin/stdout and returns a buffered tube."
   tube.process(path, args, level, timeout_ms, chunk_size)
}

fn process(str path, list args=[], any level="", int timeout_ms=-1, int chunk_size=0) any {
   "Starts a local process with piped stdin/stdout and returns a buffered tube."
   tube.process(path, args, level, timeout_ms, chunk_size)
}

fn shell(str command, any level="", int timeout_ms=-1, int chunk_size=0) any {
   "Starts the platform shell and returns a buffered process tube."
   tube.shell(command, level, timeout_ms, chunk_size)
}

fn ssh(str host, any user="", int port=22, any command="", list options=[], any level="", int timeout_ms=-1, int chunk_size=0) any {
   "Starts the local OpenSSH client as a buffered process tube."
   tube.ssh(host, user, port, command, options, level, timeout_ms, chunk_size)
}

fn ssh_process(str host, list command=[], any user="", int port=22, list options=[], any level="", int timeout_ms=-1, int chunk_size=0) any {
   "Starts an SSH command tube using a remote argv-style command list."
   tube.ssh_process(host, command, user, port, options, level, timeout_ms, chunk_size)
}

fn ssh_shell(str host, any user="", int port=22, list options=[], any level="", int timeout_ms=-1, int chunk_size=0) any {
   "Starts an interactive SSH shell tube using the local OpenSSH client."
   tube.ssh_shell(host, user, port, options, level, timeout_ms, chunk_size)
}

fn dial(str host, int port, any level="", int timeout_ms=-1, int chunk_size=0) any {
   "Connects to a TCP endpoint and returns a buffered tube."
   tube.remote(host, port, level, timeout_ms, chunk_size)
}

fn remote(str host, int port, any level="", int timeout_ms=-1, int chunk_size=0) any {
   "Alias for `dial(host, port)`."
   tube.remote(host, port, level, timeout_ms, chunk_size)
}

fn connect(str host, int port, any level="", int timeout_ms=-1, int chunk_size=0) any {
   "Alias for `dial(host, port)`."
   tube.connect(host, port, level, timeout_ms, chunk_size)
}

fn connect_retry(str host, int port, int retries=20, int delay_ms=50, any level="", int timeout_ms=-1, int chunk_size=0) any {
   "Connects with retries and returns a buffered tube."
   tube.connect_retry(host, port, retries, delay_ms, level, timeout_ms, chunk_size)
}

fn context(any log_level="") dict { tube.context(log_level) }

fn set_context(any options=0) dict { tube.set_context(options) }

fn verbose(any io, bool on=true) any { tube.set_verbose(io, on) }

fn set_verbose(any io, bool on=true) any { tube.set_verbose(io, on) }

fn set_level(any io, any level="debug") any { tube.set_level(io, level) }

fn set_log_level(any io, any level="debug") any { tube.set_log_level(io, level) }

fn log_level(any io) str { tube.log_level(io) }

fn set_timeout(any io, int timeout_ms) any { tube.set_timeout(io, timeout_ms) }

fn interactive(any io, int max_read=4096) int { tube.interactive(io, max_read) }

fn send(any io, any data) int { tube.send(io, data) }

fn sendline(any io, any data="") int { tube.sendline(io, data) }

fn recv(any io, any n=4096) str { tube.recv(io, n) }

fn recvn(any io, int n=4096) str { tube.recvn(io, n) }

fn recv_line(any io, bool keepends=true, int max_bytes=65536) str { tube.recv_line(io, keepends, max_bytes) }

fn recvline(any io, bool keepends=true, int max_bytes=65536) str { tube.recvline(io, keepends, max_bytes) }

fn recv_all(any io, int max_bytes=65536) str { tube.recv_all(io, max_bytes) }

fn recvall(any io, int max_bytes=65536) str { tube.recvall(io, max_bytes) }

fn recv_until(any io, any needle, bool drop=false, int max_bytes=65536) str { tube.recv_until(io, needle, drop, max_bytes) }

fn recvuntil(any io, any needle, bool drop=false, int max_bytes=65536) str { tube.recvuntil(io, needle, drop, max_bytes) }

fn expect(any io, any needles, int max_bytes=65536) list { tube.expect(io, needles, max_bytes) }

fn expect_map(any io, any mapping, int max_bytes=65536) list { tube.expect_map(io, mapping, max_bytes) }

fn send_after(any io, any needle, any data, int max_bytes=65536) str { tube.send_after(io, needle, data, max_bytes) }

fn sendafter(any io, any needle, any data, int max_bytes=65536) str { tube.sendafter(io, needle, data, max_bytes) }

fn sendline_after(any io, any needle, any data, int max_bytes=65536) str { tube.sendline_after(io, needle, data, max_bytes) }

fn sendlineafter(any io, any needle, any data, int max_bytes=65536) str { tube.sendlineafter(io, needle, data, max_bytes) }

fn clean(any io) str { tube.clean(io) }

fn buffered(any io) int { tube.buffered(io) }

fn unrecv(any io, any data) int { tube.unrecv(io, data) }

fn shutdown_send(any io) int { tube.shutdown_send(io) }

fn close(any io) int { tube.close(io) }

fn connected(any io) bool { tube.connected(io) }

fn tube_kind(any io) str { tube.tube_kind(io) }

fn fileno(any io) int { tube.fileno(io) }

fn pid(any io) int { tube.pid(io) }

fn transcript(any io) list { tube.transcript(io) }

fn transcript_text(any io) str { tube.transcript_text(io) }
