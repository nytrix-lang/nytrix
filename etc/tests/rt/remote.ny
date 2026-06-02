use std.core
use std.core.test
use std.os
use std.os.interact as interact
use std.os.net.remote as tube
use std.os.net.requests as requests

fn test_tube_fuzz(){
   assert_eq(tube.set_default_level("trace"), "trace", "set_default_level trace")
   assert_eq(tube.default_level(), "trace", "default_level trace")
   def ctx = tube.context(log_level="quiet")
   assert_eq(ctx.get("log_level", ""), "quiet", "context log_level")
   mut i = 0
   while(i < 128){
      def prefix = "case=" + to_str(i)
      def line = "line-" + to_str(i)
      def payload = prefix + "|" + line + "\nneedle-" + to_str(i) + "-END!tail"
      def io = tube.tube_fd(-1, "fuzz", i, level="quiet", timeout_ms=0, chunk_size=(i % 7) + 1)
      assert_eq(tube.log_level(io), "quiet", "initial log level")
      assert_eq(io.get("timeout_ms", -1), 0, "timeout option")
      assert_eq(io.get("chunk", 0), (i % 7) + 1, "chunk option")
      assert_eq(tube.unrecv(io, payload), payload.len, "unrecv byte count")
      assert_eq(tube.buffered(io), payload.len, "buffered byte count")
      assert_eq(tube.recvuntil(io, "|"), prefix + "|", "recvuntil keeps delimiter")
      assert_eq(tube.recvline(io, false), line, "recvline can drop newline")
      def hit = tube.expect(io, ["missing", "END"])
      assert_eq(hit.get(0), 1, "expect index")
      assert_eq(hit.get(1), "needle-" + to_str(i) + "-END", "expect payload")
      assert_eq(tube.recvn(io, 5), "!tail", "recvn reads buffered suffix")
      assert_eq(tube.buffered(io), 0, "buffer empty after reads")
      assert_eq(tube.unrecv(io, "tail"), 4, "unrecv tail")
      assert_eq(tube.unrecv(io, "head"), 4, "unrecv head")
      assert_eq(tube.recvn(io, 8), "headtail", "unrecv prepends data")
      assert_eq(tube.clean(io), "", "clean empty")
      assert_eq(tube.send(io, "x"), -1, "send fails on disconnected tube")
      assert_eq(tube.close(io), 0, "close disconnected tube")
      i += 1
   }
   def dbg = tube.tube_fd(-1, "fuzz", 999, level="debug", timeout_ms=0)
   assert_eq(tube.log_level(dbg), "debug", "initial debug level")
   tube.set_level(dbg, "error")
   assert_eq(tube.log_level(dbg), "error", "set_level error")
   tube.set_verbose(dbg, false)
   assert_eq(tube.log_level(dbg), "quiet", "set_verbose false")
   assert_eq(tube.close(dbg), 0, "close debug tube")
}

fn test_fixture_sendafter(){
   def io = tube.fixture("name: ", "quiet", 2)
   assert_eq(tube.tube_kind(io), "fixture", "fixture kind")
   assert_eq(tube.sendlineafter(io, "name: ", "nytrix"), "name: ", "fixture prompt")
   assert_eq(io.get("sent", ""), "nytrix\n", "fixture captures sent data")
   def rows = tube.transcript(io)
   assert(rows.len >= 2, "fixture transcript captures send/recv")
   def replay = tube.replay(rows, "quiet", 3)
   assert_eq(tube.recvall(replay), "name: ", "replay reproduces received transcript")
   assert_eq(tube.close(io), 0, "fixture close")
}

fn test_interact_facade(){
   def io = tube.fixture("ready\n", "quiet", 1)
   assert_eq(interact.tube_kind(io), "fixture", "interact facade keeps tube kind")
   assert_eq(interact.recvline(io, false), "ready", "interact facade delegates recvline")
   assert_eq(interact.sendline(io, "pong"), 5, "interact facade delegates sendline")
   assert_eq(io.get("sent", ""), "pong\n", "interact facade writes through same tube")
   assert_eq(interact.close(io), 0, "interact facade close")
}

fn test_process_tube(){
   #windows {
      print("skip process tube test on Windows")
      return 0
   } #endif
   def cmd = "printf 'ready\\n'; IFS= read line; printf 'echo:%s\\n' \"$line\""
   def io = tube.shell(cmd, "quiet", 0, 2)
   if(io == 0){
      print("skip process tube(spawn unavailable)")
      return 0
   }
   assert_eq(tube.tube_kind(io), "process", "process kind")
   assert(tube.pid(io) > 0, "process pid")
   assert_eq(tube.recvline(io, false), "ready", "process recvline")
   assert_eq(tube.sendline(io, "hello"), 6, "process sendline")
   assert_eq(tube.recvuntil(io, "\n", true), "echo:hello", "process recvuntil drop")
   assert(tube.close(io) == 0, "process close status")
   0
}

fn test_ssh_tube_smoke(){
   #windows {
      print("skip ssh tube smoke on Windows")
      return 0
   } #endif
   def io = tube.ssh("127.0.0.1", "", 1, "", ["-o", "BatchMode=yes", "-o", "ConnectTimeout=1"], "quiet", 1000, 64)
   if(io == 0){
      print("skip ssh tube smoke(spawn unavailable)")
      return 0
   }
   assert_eq(tube.tube_kind(io), "process", "ssh uses process tube")
   assert(tube.pid(io) > 0, "ssh process pid")
   def out = tube.recvall(io, 8192)
   out
   def code = tube.close(io)
   assert(code >= 0, "ssh tube exits with a process status")
   0
}

fn test_requests_parse_metadata(){
   def raw = "HTTP/1.1 201 Created\r\nContent-Length: 5\r\nX-Test: ok\r\n\r\nhelloignored"
   def r = requests.requests_parse_response(raw)
   assert_eq(r.get("ok", false), true, "parsed response ok")
   assert_eq(r.get("status", 0), 201, "parsed response status")
   assert_eq(r.get("reason", ""), "Created", "parsed response reason")
   assert_eq(r.get("body", ""), "hello", "parsed content-length body")
   def h = r.get("headers", 0)
   assert_eq(h.get("x-test", ""), "ok", "parsed header")
}

fn test_remote_fetch(){
   if(!env("NYTRIX_TEST_REMOTE")){
      print("skip remote fetch(set NYTRIX_TEST_REMOTE=1 to run)")
      return 0
   }
   print("Testing remote fetch...")
   def url = "https://example.com"
   def text = fetch(url)
   if(text == nil || text == 0){
      print("skip remote fetch(offline or libcurl unavailable)")
      return 0
   }
   def n = text.len
   print(f"Fetched URL: {url}, length: {n}")
   if(n < 10){
      print(f"fetch returned: '{text}'")
      panic(f"Failed to fetch {url} or content too short.")
   }
   print("Fetched content preview:")
   print(slice(text, 0, 60, 1) + "...")
   0
}

print("Testing remote tube fuzz...")
test_tube_fuzz()
test_fixture_sendafter()
test_interact_facade()
test_process_tube()
test_ssh_tube_smoke()
test_requests_parse_metadata()
test_remote_fetch()
print("✓ All remote tests passed")
