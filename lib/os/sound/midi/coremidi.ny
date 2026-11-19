;; Keywords: midi coremidi macos sound os
;; macOS MIDI backend over CoreMIDI.
;;
;; CoreMIDI delivers input on its own high-priority thread through a
;; MIDIReadProc. The callback here only copies packet bytes into a
;; preallocated single-producer/single-consumer slot ring (raw memory plus
;; atomic head/tail counters, no allocation, no Ny containers); `read_bytes`
;; drains the ring on the caller's thread. Long sysex packets are split
;; across slots and re-assembled by the shared byte parser.
;; References:
;; - core.os.sound.midi
;; - developer.apple.com/documentation/coremidi
module core.os.sound.midi.coremidi(available, devices, open_input, open_output, open_virtual_input, open_virtual_output, read_bytes, write_bytes, close)
use core
use core.mem
use core.str as str

#macos {
   #link "-framework CoreMIDI"
   #link "-framework CoreFoundation"
   #include <CoreMIDI/CoreMIDI.h>
   #include <CoreFoundation/CoreFoundation.h>
   extern "" {
      fn MIDIClientCreate(any name, any notify, any ctx, any out_client) i32
      fn MIDIClientDispose(u32 client) i32
      fn MIDIInputPortCreate(u32 client, any name, any read_proc, any ref_con, any out_port) i32
      fn MIDIOutputPortCreate(u32 client, any name, any out_port) i32
      fn MIDIPortDispose(u32 port) i32
      fn MIDIGetNumberOfSources() u64
      fn MIDIGetSource(u64 index) u32
      fn MIDIGetNumberOfDestinations() u64
      fn MIDIGetDestination(u64 index) u32
      fn MIDIObjectGetStringProperty(u32 obj, any prop, any out_str) i32
      fn MIDIPortConnectSource(u32 port, u32 source, any conn_ref) i32
      fn MIDIPortDisconnectSource(u32 port, u32 source) i32
      fn MIDISend(u32 port, u32 dest, any pktlist) i32
      fn MIDIReceived(u32 source, any pktlist) i32
      fn MIDIDestinationCreate(u32 client, any name, any read_proc, any ref_con, any out_dest) i32
      fn MIDISourceCreate(u32 client, any name, any out_src) i32
      fn MIDIEndpointDispose(u32 endpoint) i32
      fn CFStringCreateWithCString(any alloc, any s, u32 enc) any
      fn CFStringGetCString(any s, any buf, i64 cap, u32 enc) i32
      fn CFRelease(any obj) any
      fn usleep(u32 us) i32
   }
} #else {
   fn MIDIClientCreate(any _n, any _f, any _c, any _o) int { -1 }
   fn MIDIClientDispose(any _c) int { -1 }
   fn MIDIInputPortCreate(any _c, any _n, any _r, any _x, any _o) int { -1 }
   fn MIDIOutputPortCreate(any _c, any _n, any _o) int { -1 }
   fn MIDIPortDispose(any _p) int { -1 }
   fn MIDIGetNumberOfSources() int { 0 }
   fn MIDIGetSource(any _i) int { 0 }
   fn MIDIGetNumberOfDestinations() int { 0 }
   fn MIDIGetDestination(any _i) int { 0 }
   fn MIDIObjectGetStringProperty(any _o, any _p, any _s) int { -1 }
   fn MIDIPortConnectSource(any _p, any _s, any _r) int { -1 }
   fn MIDIPortDisconnectSource(any _p, any _s) int { -1 }
   fn MIDISend(any _p, any _d, any _l) int { -1 }
   fn MIDIReceived(any _s, any _l) int { -1 }
   fn MIDIDestinationCreate(any _c, any _n, any _r, any _x, any _o) int { -1 }
   fn MIDISourceCreate(any _c, any _n, any _o) int { -1 }
   fn MIDIEndpointDispose(any _e) int { -1 }
   fn CFStringCreateWithCString(any _a, any _s, any _e) any { 0 }
   fn CFStringGetCString(any _s, any _b, any _c, any _e) int { 0 }
   fn CFRelease(any _o) any { 0 }
   fn usleep(any _us) int { 0 }
} #endif
def _UTF8 = 0x08000100

;; Ring layout: [head u64][tail u64][dropped u64][pad] then slots.
def _RING_HEAD = 0
def _RING_TAIL = 8
def _RING_DROPPED = 16
def _RING_SLOTS = 64
def _SLOT_COUNT = 1024
def _SLOT_DATA = 256
def _SLOT_SIZE = 272
def _SLOT_LEN = 0
def _SLOT_SRC = 4
def _SLOT_BYTES = 8
def _PKT_DATA = 10
def _PKT_LEN = 8
mut _client = 0
mut _ring = 0
mut _pending = dict(8)

fn available() bool {
   "Reports whether CoreMIDI is present."
   #macos { true } #else { false } #endif
}

fn _cfstr(str s) any { CFStringCreateWithCString(0, __any_to_cstr(s), _UTF8) }

fn _endpoint_name(int endpoint) str {
   def prop = _cfstr("displayName")
   def outp = zalloc(8)
   mut name = ""
   if int(MIDIObjectGetStringProperty(endpoint, prop, outp)) == 0 {
      def cf = load64(outp, 0)
      def buf = zalloc(256)
      if int(CFStringGetCString(cf, buf, 256, _UTF8)) != 0 { name = str.cstr_to_str(buf, 0) }
      free(buf)
      CFRelease(cf)
   }
   free(outp)
   CFRelease(prop)
   name
}

fn _ensure_client() bool {
   if _client != 0 { return true }
   if _ring == 0 { _ring = zalloc(_RING_SLOTS + _SLOT_COUNT * _SLOT_SIZE) }
   def name = _cfstr("Nytrix MIDI")
   def outp = zalloc(8)
   def rc = int(MIDIClientCreate(name, 0, 0, outp))
   if rc == 0 { _client = load32(outp, 0) }
   free(outp)
   CFRelease(name)
   _client != 0
}

fn devices() list {
   "Lists CoreMIDI sources(inputs) and destinations(outputs)."
   mut out = []
   def ns = int(MIDIGetNumberOfSources())
   mut i = 0
   while i < ns {
      def ep = int(MIDIGetSource(i))
      out = out.append({"id": "coremidi:in:" + to_str(i), "index": i, "endpoint": ep, "name": _endpoint_name(ep), "input": true, "output": false, "backend": "coremidi"})
      i += 1
   }
   def nd = int(MIDIGetNumberOfDestinations())
   i = 0
   while i < nd {
      def ep = int(MIDIGetDestination(i))
      out = out.append({"id": "coremidi:out:" + to_str(i), "index": i, "endpoint": ep, "name": _endpoint_name(ep), "input": false, "output": true, "backend": "coremidi"})
      i += 1
   }
   out
}

fn _read_proc(any pktlist, any read_ref, any src_ref) any {
   ;; Runs on the CoreMIDI thread: raw loads/stores and atomics only.
   def ring = _ring
   if !ring || !pktlist { return 0 }
   def count = load32(pktlist, 0)
   mut pkt = pktlist + 4
   mut k = 0
   while k < count {
      def len = load16(pkt, _PKT_LEN)
      mut off = 0
      while off < len {
         def head = __atomic_load64(ring, _RING_HEAD)
         def tail = __atomic_load64(ring, _RING_TAIL)
         if head - tail >= _SLOT_COUNT {
            __atomic_store64(ring, _RING_DROPPED, __atomic_load64(ring, _RING_DROPPED) + 1)
            off = len
         } else {
            def slot = ring + _RING_SLOTS + (head % _SLOT_COUNT) * _SLOT_SIZE
            def chunk = len - off < _SLOT_DATA ? len - off : _SLOT_DATA
            memcpy(slot + _SLOT_BYTES, pkt + _PKT_DATA + off, chunk)
            store32(slot, chunk, _SLOT_LEN)
            store32(slot, int(src_ref), _SLOT_SRC)
            __atomic_store64(ring, _RING_HEAD, head + 1)
            off += chunk
         }
      }
      ;; MIDIPacketNext: arm64 aligns packets to 4 bytes, x86_64 packs them.
      #aarch64 {
         pkt = (pkt + _PKT_DATA + len + 3) & ~3
      } #else {
         pkt = pkt + _PKT_DATA + len
      } #endif
      k += 1
   }
   0
}

fn open_input(int index) any {
   "Connects CoreMIDI source `index` to a new input port."
   if !_ensure_client() { return 0 }
   def src = int(MIDIGetSource(index))
   if src == 0 { return 0 }
   def name = _cfstr("in")
   def outp = zalloc(8)
   def rc = int(MIDIInputPortCreate(_client, name, _read_proc, 0, outp))
   def port = rc == 0 ? load32(outp, 0) : 0
   free(outp)
   CFRelease(name)
   if port == 0 { return 0 }
   if int(MIDIPortConnectSource(port, src, index + 1)) != 0 {
      MIDIPortDispose(port)
      return 0
   }
   {"kind": "in", "port": port, "source": src, "tag": index + 1, "open": true}
}

fn open_virtual_input(str name) any {
   "Creates a virtual destination other apps can send to."
   if !_ensure_client() { return 0 }
   def cf = _cfstr(name)
   def outp = zalloc(8)
   def rc = int(MIDIDestinationCreate(_client, cf, _read_proc, 0, outp))
   def ep = rc == 0 ? load32(outp, 0) : 0
   free(outp)
   CFRelease(cf)
   if ep == 0 { return 0 }
   {"kind": "in", "endpoint": ep, "virtual": true, "tag": 0, "open": true}
}

fn open_output(int index) any {
   "Opens an output port to CoreMIDI destination `index`."
   if !_ensure_client() { return 0 }
   def dest = int(MIDIGetDestination(index))
   if dest == 0 { return 0 }
   def name = _cfstr("out")
   def outp = zalloc(8)
   def rc = int(MIDIOutputPortCreate(_client, name, outp))
   def port = rc == 0 ? load32(outp, 0) : 0
   free(outp)
   CFRelease(name)
   if port == 0 { return 0 }
   {"kind": "out", "port": port, "dest": dest, "open": true}
}

fn open_virtual_output(str name) any {
   "Creates a virtual source other apps can receive from."
   if !_ensure_client() { return 0 }
   def cf = _cfstr(name)
   def outp = zalloc(8)
   def rc = int(MIDISourceCreate(_client, cf, outp))
   def ep = rc == 0 ? load32(outp, 0) : 0
   free(outp)
   CFRelease(cf)
   if ep == 0 { return 0 }
   {"kind": "out", "endpoint": ep, "virtual": true, "open": true}
}

fn read_bytes(any stream, int timeout_ms=0) list {
   "Drains ring slots that belong to `stream` and returns byte chunks."
   if !is_dict(stream) || !bool(stream.get("open", false)) || _ring == 0 { return [] }
   def want = int(stream.get("tag", 0))
   mut waited = 0
   while timeout_ms != 0 && __atomic_load64(_ring, _RING_HEAD) == __atomic_load64(_ring, _RING_TAIL) {
      if timeout_ms > 0 && waited >= timeout_ms { break }
      usleep(1000)
      waited += 1
   }
   ;; One ring serves every stream: drain it into per-stream queues keyed
   ;; by the connection tag, then hand back only this stream's chunks.
   mut tail = __atomic_load64(_ring, _RING_TAIL)
   def head = __atomic_load64(_ring, _RING_HEAD)
   while tail < head {
      def slot = _ring + _RING_SLOTS + (tail % _SLOT_COUNT) * _SLOT_SIZE
      def len = load32(slot, _SLOT_LEN)
      def key = to_str(load32(slot, _SLOT_SRC))
      mut bytes = []
      mut i = 0
      while i < len {
         bytes = bytes.append(load8(slot, _SLOT_BYTES + i))
         i += 1
      }
      _pending[key] = _pending.get(key, []).append(bytes)
      tail += 1
   }
   __atomic_store64(_ring, _RING_TAIL, tail)
   def mine = to_str(want)
   def out = _pending.get(mine, [])
   _pending = _pending.delete(mine)
   out
}

fn write_bytes(any stream, list bytes) bool {
   "Sends one message as a single-packet MIDIPacketList."
   if !is_dict(stream) || !bool(stream.get("open", false)) || bytes.len == 0 || bytes.len > 65000 { return false }
   def list_p = zalloc(4 + _PKT_DATA + bytes.len + 8)
   store32(list_p, 1, 0)
   def pkt = list_p + 4
   store16(pkt, bytes.len, _PKT_LEN)
   mut i = 0
   while i < bytes.len {
      store8(pkt, int(bytes.get(i, 0)) & 255, _PKT_DATA + i)
      i += 1
   }
   mut rc = -1
   if bool(stream.get("virtual", false)) {
      rc = int(MIDIReceived(int(stream.get("endpoint", 0)), list_p))
   } else {
      rc = int(MIDISend(int(stream.get("port", 0)), int(stream.get("dest", 0)), list_p))
   }
   free(list_p)
   rc == 0
}

fn close(any stream) bool {
   "Disconnects and disposes a CoreMIDI stream."
   if !is_dict(stream) || !bool(stream.get("open", false)) { return false }
   stream["open"] = false
   if bool(stream.get("virtual", false)) {
      MIDIEndpointDispose(int(stream.get("endpoint", 0)))
      return true
   }
   def port = int(stream.get("port", 0))
   if to_str(stream.get("kind", "")) == "in" { MIDIPortDisconnectSource(port, int(stream.get("source", 0))) }
   MIDIPortDispose(port)
   true
}
