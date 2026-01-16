use std.io
use std.net.socket
use std.strings.str
use std.os.thread
use std.os.time
use std.core.test
use std.core

def PORT = 50005

fn server_task(arg){
   def port = arg
   def s = socket_bind("127.0.0.1", port)
   if(s < 0){ return -1 }
   def c = socket_accept(s)
   if(c < 0){
      close_socket(s)
      return -1
   }
   def req = read_socket(c, 1024)
   if(eq(req, "ping")){
      write_socket(c, "pong")
   }
   msleep(100)
   close_socket(c)
   close_socket(s)
   return 0
}

fn test_socket_ping_pong(){
   print("Testing Sockets...")
   def t = thread_spawn(server_task, PORT)
   msleep(500) ; Wait for server
   def c = socket_connect("127.0.0.1", PORT)
   if(c < 0){ panic("client connect failed") }
   write_socket(c, "ping")
   msleep(50) ; give server time to respond
   def res = read_socket(c, 1024)
   assert(eq(res, "pong"), "socket ping/pong")
   close_socket(c)
   thread_join(t)
   print("✓ std.net.socket passed")
}

test_socket_ping_pong()
