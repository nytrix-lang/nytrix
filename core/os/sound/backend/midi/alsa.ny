;; Keywords: midi alsa sequencer rawmidi sound backend os linux
;; Linux MIDI backend: an ALSA sequencer client written directly against the
;; kernel interface (/dev/snd/seq ioctls plus read/write of snd_seq_event
;; records, no libasound), and raw MIDI device nodes (/dev/snd/midiC*D*).
;; Sequencer events are exchanged as message dicts (see core.os.sound.midi);
;; raw devices exchange wire bytes that the front end parses.
;; References:
;; - core.os.sound.midi
;; - linux include/uapi/sound/asequencer.h
module core.os.sound.backend.midi.alsa(
   available, open_client, close_client, client_id, create_port, delete_port, devices,
   connect_input, connect_output, disconnect_input, disconnect_output, send, receive,
   raw_devices, raw_open, raw_close, raw_write_bytes, raw_read_bytes,
   PORT_INPUT, PORT_OUTPUT, PORT_DUPLEX
)

use core
use core.mem
use core.os.sys as os_sys
use core.os.fs as osfs
use core.str as str

def PORT_INPUT = 1
def PORT_OUTPUT = 2
def PORT_DUPLEX = 3

;; ALSA sequencer ABI (x86_64/aarch64 LP64; layouts checked against
;; <sound/asequencer.h>).
def _IOCTL_CLIENT_ID = 0x80045301
def _IOCTL_SET_CLIENT_INFO = 0x40bc5311
def _IOCTL_CREATE_PORT = 0xc0a85320
def _IOCTL_DELETE_PORT = 0x40a85321
def _IOCTL_SUBSCRIBE_PORT = 0x40505330
def _IOCTL_UNSUBSCRIBE_PORT = 0x40505331
def _IOCTL_QUERY_NEXT_CLIENT = 0xc0bc5351
def _IOCTL_QUERY_NEXT_PORT = 0xc0a85352
def _CLIENT_INFO_SIZE = 188
def _PORT_INFO_SIZE = 168
def _SUBSCRIBE_SIZE = 80
def _EVENT_SIZE = 28
def _CAP_READ = 1
def _CAP_WRITE = 2
def _CAP_SUBS_READ = 0x20
def _CAP_SUBS_WRITE = 0x40
def _CAP_NO_EXPORT = 0x80
def _TYPE_MIDI_GENERIC = 2
def _TYPE_APPLICATION = 0x100000
def _ADDRESS_SUBSCRIBERS = 254
def _ADDRESS_UNKNOWN = 253
def _QUEUE_DIRECT = 253
def _FLAG_VARIABLE = 4
def _EXT_LEN_MASK = 0x3fffffff
def _O_RDWR = 2
def _O_WRONLY = 1
def _O_RDONLY = 0
def _O_NONBLOCK = 0x800
def _O_CLOEXEC = 0x80000
def _POLLIN = 1
def _READ_BUF = 8192

;; Sequencer event type numbers.
def _EV_NOTEON = 6
def _EV_NOTEOFF = 7
def _EV_KEYPRESS = 8
def _EV_CONTROLLER = 10
def _EV_PGMCHANGE = 11
def _EV_CHANPRESS = 12
def _EV_PITCHBEND = 13
def _EV_SONGPOS = 20
def _EV_SONGSEL = 21
def _EV_QFRAME = 22
def _EV_START = 30
def _EV_CONTINUE = 31
def _EV_STOP = 32
def _EV_CLOCK = 36
def _EV_TUNE_REQUEST = 40
def _EV_RESET = 41
def _EV_SENSING = 42
def _EV_SYSEX = 130

fn available() bool {
   "Reports whether this build targets the ALSA sequencer."
   #linux { return true } #endif
   false
}

fn _clamp7(any v) int {
   def i = int(v)
   if i < 0 { return 0 }
   if i > 127 { return 127 }
   i
}

fn _chan(any c) int { int(c) & 15 }

fn _sysex_bytes(any data) list {
   ;; F0 ... F7 framing; the last byte is tracked locally (see the note in
   ;; core.os.sound.midi.sysex about raw-int list reads).
   mut bytes = []
   def src = is_list(data) ? data : []
   mut last = -1
   if src.len == 0 || int(src.get(0, 0)) != 0xF0 {
      bytes = bytes.append(0xF0)
      last = 0xF0
   }
   mut i = 0
   while i < src.len {
      last = int(src.get(i, 0)) & 255
      bytes = bytes.append(last)
      i += 1
   }
   if last != 0xF7 { bytes = bytes.append(0xF7) }
   bytes
}

;; ---------------------------------------------------------------------------
;; ALSA sequencer client
fn _ioctl(int fd, int req, any arg) int {
   ;; rt_ioctl untags every odd scalar argument, so a raw fd of 3 or an odd
   ;; request number such as CLIENT_ID (0x80045301) would be halved. Pass
   ;; both as tagged ints so the untag is exact for odd and even values.
   int(__ioctl(__tag(fd), __tag(req & 0xffffffff), arg))
}

fn _store_name(any p, int off, str name, int cap) any {
   mut i = 0
   def n = name.len < cap - 1 ? name.len : cap - 1
   while i < n {
      store8(p, load8(name, i), off + i)
      i += 1
   }
   store8(p, 0, off + n)
   0
}

fn _read_name(any p, int off, int cap) str {
   ;; Kernel names are NUL-terminated within `cap` bytes; force the
   ;; terminator so a full-width name cannot run past the field.
   store8(p, 0, off + cap - 1)
   str.cstr_to_str(p, off)
}

fn open_client(str name="nytrix") any {
   "Opens an ALSA sequencer client. Returns a client dict, or 0 when the
   sequencer is unavailable(check `client['error']` on the dict path)."
   def fd = __open(__any_to_cstr("/dev/snd/seq"), _O_RDWR | _O_NONBLOCK | _O_CLOEXEC, 0)
   if fd < 0 { return 0 }
   def idp = zalloc_or_panic(8, "alsa client id")
   if _ioctl(fd, _IOCTL_CLIENT_ID, idp) < 0 {
      free(idp)
      os_sys.sys_close_quiet(fd)
      return 0
   }
   def id = load32(idp, 0)
   free(idp)
   def info = zalloc_or_panic(_CLIENT_INFO_SIZE, "alsa client info")
   store32(info, id, 0)
   store32(info, 1, 4)
   _store_name(info, 8, name, 64)
   if _ioctl(fd, _IOCTL_SET_CLIENT_INFO, info) < 0 { free(info) } else { free(info) }
   return {"fd": fd, "id": id, "name": name, "buf": zalloc_or_panic(_READ_BUF, "alsa event buffer"), "sysex": dict(8), "open": true}
}

fn client_id(any client) int {
   "Returns the sequencer client number."
   is_dict(client) ? int(client.get("id", -1)) : -1
}

fn close_client(any client) bool {
   "Closes a client and releases its ports and buffers."
   if !is_dict(client) || !bool(client.get("open", false)) { return false }
   os_sys.sys_close_quiet(int(client.get("fd", -1)))
   def buf = client.get("buf", 0)
   if buf { free(buf) }
   client["open"] = false
   true
}

fn create_port(any client, str name, int direction=PORT_DUPLEX) int {
   "Creates a port other programs can connect to. `direction` is PORT_INPUT
   (we receive), PORT_OUTPUT(we send), or PORT_DUPLEX. Returns the port
   number or -1."
   if !is_dict(client) { return -1 }
   mut caps = 0
   if (direction & PORT_INPUT) != 0 { caps = caps | _CAP_WRITE | _CAP_SUBS_WRITE }
   if (direction & PORT_OUTPUT) != 0 { caps = caps | _CAP_READ | _CAP_SUBS_READ }
   def info = zalloc_or_panic(_PORT_INFO_SIZE, "alsa port info")
   store8(info, int(client.get("id", 0)), 0)
   _store_name(info, 2, name, 64)
   store32(info, caps, 68)
   store32(info, _TYPE_MIDI_GENERIC | _TYPE_APPLICATION, 72)
   store32(info, 16, 76)
   def rc = _ioctl(int(client.get("fd", -1)), _IOCTL_CREATE_PORT, info)
   def port = rc < 0 ? -1 : load8(info, 1)
   free(info)
   port
}

fn delete_port(any client, int port) bool {
   "Deletes a port created with `create_port`."
   if !is_dict(client) { return false }
   def info = zalloc_or_panic(_PORT_INFO_SIZE, "alsa port info")
   store8(info, int(client.get("id", 0)), 0)
   store8(info, port, 1)
   def rc = _ioctl(int(client.get("fd", -1)), _IOCTL_DELETE_PORT, info)
   free(info)
   rc >= 0
}

fn devices(any client=0) list {
   "Enumerates sequencer ports as dicts {client, port, name, client_name,
   input, output}. `input` means the port can be read(a source to connect
   to `connect_input`), `output` means it accepts events."
   mut own = client
   def opened = !is_dict(client)
   if opened { own = open_client("nytrix-enum") }
   if !is_dict(own) { return [] }
   def fd = int(own.get("fd", -1))
   mut out = []
   def cinfo = zalloc_or_panic(_CLIENT_INFO_SIZE, "alsa client query")
   def pinfo = zalloc_or_panic(_PORT_INFO_SIZE, "alsa port query")
   store32(cinfo, -1, 0)
   while _ioctl(fd, _IOCTL_QUERY_NEXT_CLIENT, cinfo) >= 0 {
      def cid = load32(cinfo, 0)
      def cname = _read_name(cinfo, 8, 64)
      memset(pinfo, 0, _PORT_INFO_SIZE)
      store8(pinfo, cid, 0)
      store8(pinfo, 255, 1)
      while _ioctl(fd, _IOCTL_QUERY_NEXT_PORT, pinfo) >= 0 {
         def caps = load32(pinfo, 68)
         if (caps & _CAP_NO_EXPORT) == 0 {
            out = out.append({
                  "client": cid,
                  "port": load8(pinfo, 1),
                  "name": _read_name(pinfo, 2, 64),
                  "client_name": cname,
                  "input": (caps & (_CAP_READ | _CAP_SUBS_READ)) == (_CAP_READ | _CAP_SUBS_READ),
                  "output": (caps & (_CAP_WRITE | _CAP_SUBS_WRITE)) == (_CAP_WRITE | _CAP_SUBS_WRITE),
                  "kind": "seq"
               })
         }
         store8(pinfo, cid, 0)
      }
   }
   free(cinfo)
   free(pinfo)
   if opened { close_client(own) }
   out
}

fn _subscribe(any client, int req, int sc, int sp, int dc, int dp) bool {
   if !is_dict(client) { return false }
   def sub = zalloc_or_panic(_SUBSCRIBE_SIZE, "alsa subscribe request")
   store8(sub, sc, 0)
   store8(sub, sp, 1)
   store8(sub, dc, 2)
   store8(sub, dp, 3)
   def rc = _ioctl(int(client.get("fd", -1)), req, sub)
   free(sub)
   rc >= 0
}

fn connect_input(any client, int port, int src_client, int src_port) bool {
   "Routes events from a source port into one of our input ports."
   _subscribe(client, _IOCTL_SUBSCRIBE_PORT, src_client, src_port, client_id(client), port)
}

fn connect_output(any client, int port, int dst_client, int dst_port) bool {
   "Routes events from one of our output ports to a destination port."
   _subscribe(client, _IOCTL_SUBSCRIBE_PORT, client_id(client), port, dst_client, dst_port)
}

fn disconnect_input(any client, int port, int src_client, int src_port) bool {
   "Removes a connection made with `connect_input`."
   _subscribe(client, _IOCTL_UNSUBSCRIBE_PORT, src_client, src_port, client_id(client), port)
}

fn disconnect_output(any client, int port, int dst_client, int dst_port) bool {
   "Removes a connection made with `connect_output`."
   _subscribe(client, _IOCTL_UNSUBSCRIBE_PORT, client_id(client), port, dst_client, dst_port)
}

fn _event_fields(dict msg) list {
   "Returns [event_type, byte0, byte1, byte2, param, value] for a message."
   def kind = to_str(msg.get("type", ""))
   def ch = _chan(msg.get("channel", 0))
   match kind {
      "note_on" -> { return [_EV_NOTEON, ch, _clamp7(msg.get("note", 0)), _clamp7(msg.get("velocity", 0)), 0, 0] }
      "note_off" -> { return [_EV_NOTEOFF, ch, _clamp7(msg.get("note", 0)), _clamp7(msg.get("velocity", 0)), 0, 0] }
      "poly_pressure" -> { return [_EV_KEYPRESS, ch, _clamp7(msg.get("note", 0)), _clamp7(msg.get("pressure", 0)), 0, 0] }
      "control_change" -> { return [_EV_CONTROLLER, ch, 0, 0, _clamp7(msg.get("controller", 0)), _clamp7(msg.get("value", 0))] }
      "program_change" -> { return [_EV_PGMCHANGE, ch, 0, 0, 0, _clamp7(msg.get("program", 0))] }
      "channel_pressure" -> { return [_EV_CHANPRESS, ch, 0, 0, 0, _clamp7(msg.get("pressure", 0))] }
      "pitch_bend" -> { return [_EV_PITCHBEND, ch, 0, 0, 0, int(msg.get("value", 0))] }
      "song_position" -> { return [_EV_SONGPOS, 0, 0, 0, 0, int(msg.get("value", 0))] }
      "song_select" -> { return [_EV_SONGSEL, 0, 0, 0, 0, int(msg.get("value", 0))] }
      "mtc_quarter" -> { return [_EV_QFRAME, 0, 0, 0, 0, int(msg.get("value", 0))] }
      "start" -> { return [_EV_START, 0, 0, 0, 0, 0] }
      "continue" -> { return [_EV_CONTINUE, 0, 0, 0, 0, 0] }
      "stop" -> { return [_EV_STOP, 0, 0, 0, 0, 0] }
      "clock" -> { return [_EV_CLOCK, 0, 0, 0, 0, 0] }
      "tune_request" -> { return [_EV_TUNE_REQUEST, 0, 0, 0, 0, 0] }
      "reset" -> { return [_EV_RESET, 0, 0, 0, 0, 0] }
      "active_sensing" -> { return [_EV_SENSING, 0, 0, 0, 0, 0] }
      _ -> {}
   }
   [-1, 0, 0, 0, 0, 0]
}

fn send(any client, int port, dict msg, int dst_client=-1, int dst_port=0) bool {
   "Sends a message from one of our ports. With `dst_client` < 0 the event
   goes to every subscriber of the port, otherwise directly to that address."
   if !is_dict(client) || !bool(client.get("open", false)) { return false }
   def kind = to_str(msg.get("type", ""))
   mut payload = []
   mut fields = [-1, 0, 0, 0, 0, 0]
   if kind == "sysex" {
      payload = _sysex_bytes(msg.get("data", []))
      fields = [_EV_SYSEX, 0, 0, 0, 0, 0]
   } else {
      fields = _event_fields(msg)
   }
   def ev_type = int(fields.get(0, -1))
   if ev_type < 0 { return false }
   def total = _EVENT_SIZE + payload.len
   def ev = zalloc_or_panic(total + 8, "alsa event")
   store8(ev, ev_type, 0)
   store8(ev, payload.len > 0 ? _FLAG_VARIABLE : 0, 1)
   store8(ev, _QUEUE_DIRECT, 3)
   store8(ev, client_id(client), 12)
   store8(ev, port, 13)
   if dst_client < 0 {
      store8(ev, _ADDRESS_SUBSCRIBERS, 14)
      store8(ev, _ADDRESS_UNKNOWN, 15)
   } else {
      store8(ev, dst_client, 14)
      store8(ev, dst_port, 15)
   }
   if payload.len > 0 {
      store32(ev, payload.len, 16)
      mut i = 0
      while i < payload.len {
         store8(ev, int(payload.get(i, 0)) & 255, _EVENT_SIZE + i)
         i += 1
      }
   } elif ev_type == _EV_NOTEON || ev_type == _EV_NOTEOFF || ev_type == _EV_KEYPRESS {
      store8(ev, int(fields.get(1, 0)), 16)
      store8(ev, int(fields.get(2, 0)), 17)
      store8(ev, int(fields.get(3, 0)), 18)
   } else {
      store8(ev, int(fields.get(1, 0)), 16)
      store32(ev, int(fields.get(4, 0)), 20)
      store32(ev, int(fields.get(5, 0)) & 0xffffffff, 24)
   }
   def wrote = __write_fd_raw(int(client.get("fd", -1)), ev, total)
   free(ev)
   wrote == total
}

fn _s32(int v) int { v >= 0x80000000 ? v - 0x100000000 : v }

fn _decode_event(any p, int off) dict {
   def t = load8(p, off)
   def ch = load8(p, off + 16) & 15
   def value = _s32(load32(p, off + 24))
   match t {
      6 -> { return {"type": "note_on", "channel": ch, "note": load8(p, off + 17), "velocity": load8(p, off + 18)} }
      7 -> { return {"type": "note_off", "channel": ch, "note": load8(p, off + 17), "velocity": load8(p, off + 18)} }
      8 -> { return {"type": "poly_pressure", "channel": ch, "note": load8(p, off + 17), "pressure": load8(p, off + 18)} }
      10 -> { return {"type": "control_change", "channel": ch, "controller": load32(p, off + 20), "value": value} }
      11 -> { return {"type": "program_change", "channel": ch, "program": value} }
      12 -> { return {"type": "channel_pressure", "channel": ch, "pressure": value} }
      13 -> { return {"type": "pitch_bend", "channel": ch, "value": value} }
      20 -> { return {"type": "song_position", "value": value} }
      21 -> { return {"type": "song_select", "value": value} }
      22 -> { return {"type": "mtc_quarter", "value": value} }
      30 -> { return {"type": "start"} }
      31 -> { return {"type": "continue"} }
      32 -> { return {"type": "stop"} }
      36 -> { return {"type": "clock"} }
      40 -> { return {"type": "tune_request"} }
      41 -> { return {"type": "reset"} }
      42 -> { return {"type": "active_sensing"} }
      _ -> {}
   }
   {"type": "seq_event", "event": t}
}

fn _wait_readable(int fd, int timeout_ms) bool {
   def pfd = zalloc_or_panic(8, "alsa poll descriptor")
   store32(pfd, fd, 0)
   store16(pfd, _POLLIN, 4)
   def rc = os_sys.syscall(7, pfd, 1, timeout_ms)
   def rev = load16(pfd, 6)
   free(pfd)
   rc > 0 && (rev & _POLLIN) != 0
}

fn _filtered(any filter, str kind) bool {
   if !is_list(filter) { return false }
   mut i = 0
   while i < filter.len {
      if to_str(filter.get(i, "")) == kind { return true }
      i += 1
   }
   false
}

fn receive(any client, int timeout_ms=0, any filter=0) list {
   "Returns pending messages, waiting up to `timeout_ms` for the first one
   (0 polls, negative waits forever). `filter` is a list of type names to drop."
   if !is_dict(client) || !bool(client.get("open", false)) { return [] }
   def fd = int(client.get("fd", -1))
   def buf = client.get("buf", 0)
   mut out = []
   if timeout_ms != 0 && !_wait_readable(fd, timeout_ms) { return out }
   mut pending = client.get("sysex", dict(8))
   while true {
      def n = __read_fd_raw(fd, buf, _READ_BUF)
      if n <= 0 { break }
      def now = __monotonic_ns_raw()
      mut off = 0
      while off + _EVENT_SIZE <= n {
         def t = load8(buf, off)
         def flags = load8(buf, off + 1)
         def src = [load8(buf, off + 12), load8(buf, off + 13)]
         def dst_port = load8(buf, off + 15)
         mut step = _EVENT_SIZE
         if (flags & 12) == _FLAG_VARIABLE {
            def len = load32(buf, off + 16) & _EXT_LEN_MASK
            step = _EVENT_SIZE + len
            if t == _EV_SYSEX {
               ;; The kernel may split long sysex into several events;
               ;; stitch them per source until the F7 terminator arrives.
               def key = to_str(src.get(0, 0)) + ":" + to_str(src.get(1, 0))
               mut acc = pending.get(key, [])
               mut last = -1
               mut i = 0
               while i < len && off + _EVENT_SIZE + i < n {
                  last = load8(buf, off + _EVENT_SIZE + i)
                  acc = acc.append(last)
                  i += 1
               }
               if last == 0xF7 {
                  pending = pending.delete(key)
                  if !_filtered(filter, "sysex") {
                     out = out.append({"type": "sysex", "data": acc, "time_ns": now, "source": src, "port": dst_port})
                  }
               } else {
                  pending[key] = acc
               }
            }
         } else {
            def msg = _decode_event(buf, off)
            def kind = to_str(msg.get("type", ""))
            if kind != "seq_event" && !_filtered(filter, kind) {
               msg["time_ns"] = now
               msg["source"] = src
               msg["port"] = dst_port
               out = out.append(msg)
            }
         }
         off += step
      }
   }
   client["sysex"] = pending
   out
}

;; ---------------------------------------------------------------------------
;; Raw MIDI devices (/dev/snd/midiC*D*): byte streams; the front end parses.
fn raw_devices() list {
   "Lists raw MIDI device nodes as dicts {path, card, device, kind}."
   mut out = []
   def names = osfs.list_dir("/dev/snd")
   if !is_list(names) { return out }
   mut i = 0
   while i < names.len {
      def name = to_str(names.get(i, ""))
      if str.startswith(name, "midiC") {
         def rest = str.str_slice(name, 5, name.len)
         def parts = str.split(rest, "D")
         out = out.append({"path": "/dev/snd/" + name, "card": int(parts.get(0, "0")), "device": int(parts.get(1, "0")), "kind": "raw"})
      }
      i += 1
   }
   out
}

fn raw_open(str path, int direction=PORT_DUPLEX) any {
   "Opens a raw MIDI node. Returns a handle dict or 0."
   mut flags = _O_RDWR
   if direction == PORT_INPUT { flags = _O_RDONLY }
   if direction == PORT_OUTPUT { flags = _O_WRONLY }
   def fd = __open(__any_to_cstr(path), flags | _O_NONBLOCK | _O_CLOEXEC, 0)
   if fd < 0 { return 0 }
   return {"fd": fd, "path": path, "buf": zalloc_or_panic(1024, "alsa raw read buffer"), "open": true}
}

fn raw_close(any handle) bool {
   "Closes a raw MIDI handle."
   if !is_dict(handle) || !bool(handle.get("open", false)) { return false }
   os_sys.sys_close_quiet(int(handle.get("fd", -1)))
   def buf = handle.get("buf", 0)
   if buf { free(buf) }
   handle["open"] = false
   true
}

fn raw_write_bytes(any handle, list bytes) bool {
   "Writes wire bytes to a raw MIDI handle."
   if !is_dict(handle) || !bool(handle.get("open", false)) || bytes.len == 0 { return false }
   def p = zalloc_or_panic(bytes.len + 8, "alsa raw write buffer")
   mut i = 0
   while i < bytes.len {
      store8(p, int(bytes.get(i, 0)) & 255, i)
      i += 1
   }
   def wrote = __write_fd_raw(int(handle.get("fd", -1)), p, bytes.len)
   free(p)
   wrote == bytes.len
}

fn raw_read_bytes(any handle, int timeout_ms=0) list {
   "Reads pending bytes from a raw MIDI handle as a list of byte chunks."
   if !is_dict(handle) || !bool(handle.get("open", false)) { return [] }
   def fd = int(handle.get("fd", -1))
   if timeout_ms != 0 && !_wait_readable(fd, timeout_ms) { return [] }
   def buf = handle.get("buf", 0)
   mut out = []
   while true {
      def n = __read_fd_raw(fd, buf, 1024)
      if n <= 0 { break }
      mut bytes = []
      mut i = 0
      while i < n {
         bytes = bytes.append(load8(buf, i))
         i += 1
      }
      out = out.append(bytes)
   }
   out
}
