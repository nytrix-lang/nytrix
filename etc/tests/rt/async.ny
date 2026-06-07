use std.os.async as aio
use std.os.async (async, await)
use std.os (async as os_async, await as os_await, await_all as os_await_all, ticks)
use std.os.net.socket as sock

assert(aio.backend() == "stackless", "async backend is stackless")

fn plus_one(x) { x + 1 }

fn times_two(x) { x * 2 }
def h = aio.async(plus_one, 41)
assert(aio.await(h) == 42, "std.os.async await")
def hs = [aio.future(times_two, 3), aio.Future(times_two, 5)]
assert(aio.await_all(hs) == [6, 10], "std.os.async await_all")
def oh = os_async(plus_one, 9)
assert(os_await(oh) == 10, "std.os facade await")
def ovs = os_await_all([os_async(times_two, 4), os_async(times_two, 6)])
assert(ovs == [8, 12], "std.os facade await_all")
def ch = async(plus_one, 5)
assert(await(ch) == 6, "bare async/await call compatibility")
def sh = async plus_one(41)
assert(await sh == 42, "async/await syntax")
assert(await async times_two(7) == 14, "nested async/await syntax")
def lh = async fn() { plus_one(11) }
assert(await lh == 12, "async zero-arg lambda syntax")
mut many = list(10000)
mut mi = 0
while(mi < 10000){
   many = many.append(async plus_one(mi))
   mi += 1
}

mut many_sum = 0
mi = 0
while(mi < many.len){
   many_sum += await many.get(mi)
   mi += 1
}

assert(many_sum == 50005000, "10k stackless tasks")
assert(await aio.sleep_ms(1) == 0, "stackless sleep")
assert(await aio.yield_now() == 0, "stackless yield")
assert(await __async_wait_fd(-1, 1, 0) == -1, "__async_wait_fd invalid fd")
assert(await __async_recv(-1, 0, 0, 0) == -1, "__async_recv invalid fd")
assert(await __async_send(-1, "x", 1, 0) == -1, "__async_send invalid fd")
def wait_lh = async fn() {
   await aio.yield_now()
   21
}

assert(await wait_lh == 21, "async lambda suspension")
mut waiters = list(10000)
mut wi = 0
while(wi < 10000){
   if((wi % 2) == 0){
      waiters = waiters.append(aio.yield_now())
   } else {
      waiters = waiters.append(aio.sleep_ms(0))
   }
   wi += 1
}

assert(aio.await_all(waiters).len == 10000, "10k yield/sleep tasks")

@async_effects
@effects(io, alloc, ffi)
fn effect_async_worker(base=41) {
   base + 1
}

assert(effect_async_worker() == 42, "effect-directed async value call")
def port = 55000 + ((ticks() / 1000000) % 1000)
def server = sock.socket_bind("127.0.0.1", port)

if(server >= 0){
   def accept_h = sock.socket_accept_async(server)
   def connect_h = sock.socket_connect_async("127.0.0.1", port)
   def client = await connect_h
   def peer = await accept_h
   if(client >= 0 && peer >= 0){
      assert(await sock.write_socket_all_async(client, "ping") == 4, "async socket client write")
      assert(await sock.read_socket_async(peer, 4) == "ping", "async socket server read")
      assert(await sock.write_socket_all_async(peer, "pong\n") == 5, "async socket server write")
      assert(await sock.read_socket_until_async(client, "\n", 64) == "pong\n", "async socket read until")
      sock.close_socket(client)
      sock.close_socket(peer)
   }
   def accept_h1 = sock.socket_accept_async(server)
   def accept_h2 = sock.socket_accept_async(server)
   def connect_h1 = sock.socket_connect_async("127.0.0.1", port)
   def connect_h2 = sock.socket_connect_async("127.0.0.1", port)
   def client1 = await connect_h1
   def client2 = await connect_h2
   def peer1 = await accept_h1
   def peer2 = await accept_h2
   if(client1 >= 0 && client2 >= 0 && peer1 >= 0 && peer2 >= 0){
      assert(await sock.write_socket_all_async(client1, "one") == 3, "async socket client1 write")
      assert(await sock.write_socket_all_async(client2, "two") == 3, "async socket client2 write")
      assert(await sock.read_socket_async(peer1, 3) == "one", "async socket peer1 read")
      assert(await sock.read_socket_async(peer2, 3) == "two", "async socket peer2 read")
      sock.close_socket(client1)
      sock.close_socket(client2)
      sock.close_socket(peer1)
      sock.close_socket(peer2)
   }
   sock.close_socket(server)
}

print("✓ async tests passed")
