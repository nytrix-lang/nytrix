;; Keywords: midi portmidi sysex codec alsa winmm coremidi sound os
;; Native MIDI I/O front end: message constructors, a byte-stream codec, and a
;; portable device/stream API over the OS backends in core.os.sound.backend
;; (midi.alsa on Linux, midi.winmm on Windows, midi.coremidi on macOS).
;;
;; Messages are dicts: {"type": "note_on", "channel": 0, "note": 60,
;; "velocity": 100}. Received messages also carry "time_ns" (monotonic
;; receive time) and "device" (the stream's device id). Types: note_on, note_off,
;; poly_pressure, control_change, program_change, channel_pressure,
;; pitch_bend (value -8192..8191), sysex (data: bytes including F0..F7),
;; song_position, song_select, mtc_quarter, tune_request, clock, start,
;; continue, stop, active_sensing, reset.
;;
;; Portable stream API (Linux ALSA sequencer, Windows WinMM, macOS CoreMIDI):
;;   def devs = midi.list_devices()              ;; inputs and outputs, every OS
;;   def kb = midi.open_input(midi.find_device("keystation", true))
;;   def msgs = midi.read_messages(kb, 10)       ;; wait up to 10 ms
;;   midi.write_message(out_stream, midi.note_on(0, 60, 100))
;; Sequencer-specific routing (clients, ports, subscriptions) lives in
;; core.os.sound.backend.midi.alsa.
;;
;; Portable stream API (Linux ALSA sequencer, Windows WinMM, macOS CoreMIDI):
;;   def devs = midi.list_devices()              ;; inputs and outputs, every OS
;;   def kb = midi.open_input(midi.find_device("keystation", true))
;;   def msgs = midi.read_messages(kb, 10)       ;; wait up to 10 ms
;;   midi.write_message(out_stream, midi.note_on(0, 60, 100))
;; The ALSA client API below (open_client, create_port, ...) stays available
;; for sequencer-specific routing.
;;
;; Portable stream API (Linux ALSA sequencer, Windows WinMM, macOS CoreMIDI):
;;   def devs = midi.list_devices()              ;; inputs and outputs, every OS
;;   def kb = midi.open_input(midi.find_device("keystation", true))
;;   def msgs = midi.read_messages(kb, 10)       ;; wait up to 10 ms
;;   midi.write_message(out_stream, midi.note_on(0, 60, 100))
;; The ALSA client API below (open_client, create_port, ...) stays available
;; for sequencer-specific routing.
;;
;; Portable stream API (Linux ALSA sequencer, Windows WinMM, macOS CoreMIDI):
;;   def devs = midi.list_devices()              ;; inputs and outputs, every OS
;;   def kb = midi.open_input(midi.find_device("keystation", true))
;;   def msgs = midi.read_messages(kb, 10)       ;; wait up to 10 ms
;;   midi.write_message(out_stream, midi.note_on(0, 60, 100))
;; The ALSA client API below (open_client, create_port, ...) stays available
;; for sequencer-specific routing.
;;
;; Feature matrix against PortMidi (reference: pkg.odin-lang.org/vendor/portmidi):
;;                       Linux (ALSA seq)   Windows (WinMM)   macOS (CoreMIDI)
;; - device enumeration  list_devices       list_devices      list_devices
;; - input / output      open_input/_output on every backend
;; - virtual ports       yes                no (WinMM limit)  yes
;; - timestamps          monotonic ns at receive (PortMidi Pt_Time equivalent)
;; - sysex               in and out on every backend, chunked input re-assembled
;; - filters             read_messages(stream, ms, ["clock", ...]) (Pm_SetFilter)
;; - raw device nodes    raw_open/raw_send/raw_receive (Linux /dev/snd/midiC*D*)
;; References:
;; - core.os.sound
;; - core.os.sound.backend.midi.alsa / .winmm / .coremidi
module core.os.sound.midi(
   note_on, note_off, control_change, program_change, pitch_bend, channel_pressure, poly_pressure, sysex,
   encode, parser, parse, parse_bytes, message_type_name,
   backend_name, list_devices, find_device, open_input, open_output, open_virtual_input, open_virtual_output,
   read_messages, write_message, close_stream, raw_devices, raw_open, raw_close, raw_send, raw_receive
)

use core
use core.str as str
use core.os.sound.midi.winmm as mm_winmm
use core.os.sound.midi.coremidi as mm_coremidi
use core.os.sound.midi.winmm as mm_winmm
use core.os.sound.midi.coremidi as mm_coremidi
use core.os.sound.midi.winmm as mm_winmm
use core.os.sound.midi.coremidi as mm_coremidi
use core.os.sound.backend.midi.alsa as seq
use core.os.sound.backend.midi.winmm as mm_winmm
use core.os.sound.backend.midi.coremidi as mm_coremidi

;; ---------------------------------------------------------------------------
;; Message constructors
fn _clamp7(any v) int {
   def i = int(v)
   if i < 0 { return 0 }
   if i > 127 { return 127 }
   i
}

fn _chan(any c) int { int(c) & 15 }

fn note_on(int channel, int note, int velocity=100) dict {
   "Returns a note-on message."
   {"type": "note_on", "channel": _chan(channel), "note": _clamp7(note), "velocity": _clamp7(velocity)}
}

fn note_off(int channel, int note, int velocity=0) dict {
   "Returns a note-off message."
   {"type": "note_off", "channel": _chan(channel), "note": _clamp7(note), "velocity": _clamp7(velocity)}
}

fn poly_pressure(int channel, int note, int pressure) dict {
   "Returns a polyphonic key-pressure message."
   {"type": "poly_pressure", "channel": _chan(channel), "note": _clamp7(note), "pressure": _clamp7(pressure)}
}

fn control_change(int channel, int controller, int value) dict {
   "Returns a control-change message."
   {"type": "control_change", "channel": _chan(channel), "controller": _clamp7(controller), "value": _clamp7(value)}
}

fn program_change(int channel, int program) dict {
   "Returns a program-change message."
   {"type": "program_change", "channel": _chan(channel), "program": _clamp7(program)}
}

fn channel_pressure(int channel, int pressure) dict {
   "Returns a channel-pressure(aftertouch) message."
   {"type": "channel_pressure", "channel": _chan(channel), "pressure": _clamp7(pressure)}
}

fn pitch_bend(int channel, int value) dict {
   "Returns a pitch-bend message; `value` is -8192..8191 with 0 centred."
   mut v = int(value)
   if v < -8192 { v = -8192 }
   if v > 8191 { v = 8191 }
   {"type": "pitch_bend", "channel": _chan(channel), "value": v}
}

fn sysex(any data) dict {
   "Returns a system-exclusive message. Adds the F0/F7 framing when absent."
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
   ;; Track the last byte directly: reading it back with `bytes.get` from
   ;; this raw-int list returned a mis-decoded value and doubled the F7.
   if last != 0xF7 { bytes = bytes.append(0xF7) }
   {"type": "sysex", "data": bytes}
}

;; ---------------------------------------------------------------------------
;; Byte codec
fn _simple_status(str kind) int {
   match kind {
      "tune_request" -> { return 0xF6 }
      "clock" -> { return 0xF8 }
      "start" -> { return 0xFA }
      "continue" -> { return 0xFB }
      "stop" -> { return 0xFC }
      "active_sensing" -> { return 0xFE }
      "reset" -> { return 0xFF }
      _ -> {}
   }
   -1
}

fn encode(dict msg) list {
   "Encodes a message dict as MIDI wire bytes(no running status)."
   def kind = to_str(msg.get("type", ""))
   def ch = _chan(msg.get("channel", 0))
   match kind {
      "note_on" -> { return [0x90 | ch, _clamp7(msg.get("note", 0)), _clamp7(msg.get("velocity", 0))] }
      "note_off" -> { return [0x80 | ch, _clamp7(msg.get("note", 0)), _clamp7(msg.get("velocity", 0))] }
      "poly_pressure" -> { return [0xA0 | ch, _clamp7(msg.get("note", 0)), _clamp7(msg.get("pressure", 0))] }
      "control_change" -> { return [0xB0 | ch, _clamp7(msg.get("controller", 0)), _clamp7(msg.get("value", 0))] }
      "program_change" -> { return [0xC0 | ch, _clamp7(msg.get("program", 0))] }
      "channel_pressure" -> { return [0xD0 | ch, _clamp7(msg.get("pressure", 0))] }
      "pitch_bend" -> {
         def raw = int(msg.get("value", 0)) + 8192
         return [0xE0 | ch, raw & 127, (raw >> 7) & 127]
      }
      "song_position" -> {
         def beats = int(msg.get("value", 0))
         return [0xF2, beats & 127, (beats >> 7) & 127]
      }
      "song_select" -> { return [0xF3, _clamp7(msg.get("value", 0))] }
      "mtc_quarter" -> { return [0xF1, _clamp7(msg.get("value", 0))] }
      "sysex" -> {
         def framed = sysex(msg.get("data", []))
         return framed.get("data", [])
      }
      _ -> {}
   }
   def st = _simple_status(kind)
   if st >= 0 { return [st] }
   []
}

fn _data_len(int status) int {
   "Number of data bytes that follow a status byte."
   def hi = status & 0xF0
   if hi == 0xC0 || hi == 0xD0 { return 1 }
   if hi >= 0x80 && hi <= 0xE0 { return 2 }
   if status == 0xF1 || status == 0xF3 { return 1 }
   if status == 0xF2 { return 2 }
   0
}

fn message_type_name(int status) str {
   "Returns the message type name for a status byte."
   def hi = status & 0xF0
   match hi {
      0x80 -> { return "note_off" }
      0x90 -> { return "note_on" }
      0xA0 -> { return "poly_pressure" }
      0xB0 -> { return "control_change" }
      0xC0 -> { return "program_change" }
      0xD0 -> { return "channel_pressure" }
      0xE0 -> { return "pitch_bend" }
      _ -> {}
   }
   match status {
      0xF0 -> { return "sysex" }
      0xF1 -> { return "mtc_quarter" }
      0xF2 -> { return "song_position" }
      0xF3 -> { return "song_select" }
      0xF6 -> { return "tune_request" }
      0xF8 -> { return "clock" }
      0xFA -> { return "start" }
      0xFB -> { return "continue" }
      0xFC -> { return "stop" }
      0xFE -> { return "active_sensing" }
      0xFF -> { return "reset" }
      _ -> {}
   }
   "unknown"
}

fn _decode(int status, int d1, int d2) dict {
   def kind = message_type_name(status)
   def ch = status & 15
   match kind {
      "note_on" -> { return {"type": "note_on", "channel": ch, "note": d1, "velocity": d2} }
      "note_off" -> { return {"type": "note_off", "channel": ch, "note": d1, "velocity": d2} }
      "poly_pressure" -> { return {"type": "poly_pressure", "channel": ch, "note": d1, "pressure": d2} }
      "control_change" -> { return {"type": "control_change", "channel": ch, "controller": d1, "value": d2} }
      "program_change" -> { return {"type": "program_change", "channel": ch, "program": d1} }
      "channel_pressure" -> { return {"type": "channel_pressure", "channel": ch, "pressure": d1} }
      "pitch_bend" -> { return {"type": "pitch_bend", "channel": ch, "value": (d1 | (d2 << 7)) - 8192} }
      "song_position" -> { return {"type": "song_position", "value": d1 | (d2 << 7)} }
      "song_select" -> { return {"type": "song_select", "value": d1} }
      "mtc_quarter" -> { return {"type": "mtc_quarter", "value": d1} }
      _ -> {}
   }
   {"type": kind}
}

fn parser() dict {
   "Returns a fresh streaming parser state for `parse`."
   {"running": 0, "need": 0, "data": [], "sysex": [], "in_sysex": false}
}

fn parse(dict state, any bytes) list {
   "Feeds wire bytes to a parser state and returns the completed messages.
   Handles running status, real-time bytes interleaved anywhere(including
   inside sysex), and sysex split across calls."
   mut out = []
   if !is_list(bytes) { return out }
   mut i = 0
   while i < bytes.len {
      def b = int(bytes.get(i, 0)) & 255
      i += 1
      if b >= 0xF8 {
         ;; Real-time bytes never disturb running status or a pending sysex.
         if b != 0xF9 && b != 0xFD { out = out.append({"type": message_type_name(b)}) }
         continue
      }
      if bool(state.get("in_sysex", false)) {
         if b == 0xF7 {
            state["sysex"] = state.get("sysex", []).append(0xF7)
            out = out.append({"type": "sysex", "data": state.get("sysex", [])})
            state["sysex"] = []
            state["in_sysex"] = false
            continue
         }
         if b < 0x80 {
            state["sysex"] = state.get("sysex", []).append(b)
            continue
         }
         ;; Any other status byte terminates an unfinished sysex; keep the
         ;; bytes as a framed message so data is never silently lost.
         state["sysex"] = state.get("sysex", []).append(0xF7)
         out = out.append({"type": "sysex", "data": state.get("sysex", []), "truncated": true})
         state["sysex"] = []
         state["in_sysex"] = false
      }
      if b == 0xF0 {
         state["in_sysex"] = true
         state["sysex"] = [0xF0]
         state["running"] = 0
         continue
      }
      if b >= 0x80 {
         state["data"] = []
         if b >= 0xF0 {
            ;; System common clears running status.
            state["running"] = 0
            def n = _data_len(b)
            if n == 0 {
               if b != 0xF7 && b != 0xF4 && b != 0xF5 { out = out.append({"type": message_type_name(b)}) }
               state["need"] = 0
               state["pending"] = 0
            } else {
               state["need"] = n
               state["pending"] = b
            }
         } else {
            state["running"] = b
            state["pending"] = b
            state["need"] = _data_len(b)
         }
         continue
      }
      ;; Data byte.
      mut status = int(state.get("pending", 0))
      if status == 0 {
         status = int(state.get("running", 0))
         if status == 0 { continue }
         state["pending"] = status
         state["need"] = _data_len(status)
      }
      mut data = state.get("data", [])
      data = data.append(b)
      state["data"] = data
      if data.len >= int(state.get("need", 0)) {
         out = out.append(_decode(status, int(data.get(0, 0)), int(data.get(1, 0))))
         state["data"] = []
         ;; Channel messages keep running status; system common does not.
         state["pending"] = 0
         if status >= 0xF0 { state["running"] = 0 }
      }
   }
   out
}

fn parse_bytes(any bytes) list {
   "Parses a complete byte list into messages."
   parse(parser(), bytes)
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

;; ---------------------------------------------------------------------------
;; Portable stream API
;; One shared sequencer client per process on Linux; every stream is a port.
;; Untyped dict globals: `mut any x = dict(8)` currently never holds the
;; dict (see docs/learn/quirks.md, open representation defects), and an int-initialised slot
;; mis-types a later dict, so both start as real dicts.
mut _alsa = dict(4)
mut _alsa_pending = dict(8)

fn backend_name() str {
   "Returns the MIDI backend for this OS: alsa, winmm, coremidi, or none."
   #linux { return "alsa" } #endif
   #windows { return "winmm" } #endif
   #macos { return "coremidi" } #endif
   "none"
}

fn _alsa_client() any {
   if !bool(_alsa.get("open", false)) {
      def c = seq.open_client("Nytrix")
      if is_dict(c) { _alsa = c }
   }
   bool(_alsa.get("open", false)) ? _alsa : 0
}

fn list_devices() list {
   "Lists MIDI devices on every OS as dicts {id, name, input, output,
   backend}. `input` devices can be opened with `open_input` (keyboards,
   controllers) ; `output` devices with `open_output` (synths, interfaces)."
   def b = backend_name()
   if b == "winmm" { return mm_winmm.devices() }
   if b == "coremidi" { return mm_coremidi.devices() }
   if b != "alsa" { return [] }
   def cl = _alsa_client()
   def own = seq.client_id(cl)
   def raw = seq.devices(cl)
   mut out = []
   mut i = 0
   while i < raw.len {
      def any d = raw.get(i, {})
      def c = int(d.get("client", -1))
      ;; Skip the kernel System client (timer/announce) and our own ports.
      if c != 0 && c != own && (bool(d.get("input", false)) || bool(d.get("output", false))) {
         out = out.append({
               "id": "alsa:" + to_str(c) + ":" + to_str(d.get("port", 0)),
               "name": to_str(d.get("client_name", "")) + ": " + to_str(d.get("name", "")),
               "input": bool(d.get("input", false)),
               "output": bool(d.get("output", false)),
               "backend": "alsa",
               "client": c,
               "port": int(d.get("port", 0))
            })
      }
      i += 1
   }
   out
}

fn find_device(str query, bool input=true) any {
   "Returns the first device whose id or name contains `query` (case
   insensitive) and supports the direction, or 0."
   def q = str.lower(query)
   def devs = list_devices()
   mut i = 0
   while i < devs.len {
      def any d = devs.get(i, 0)
      if !is_dict(d) {
         i += 1
         continue
      }
      def dir_ok = input ? bool(d.get("input", false)) : bool(d.get("output", false))
      if dir_ok && (q.len == 0 || str.str_contains(str.lower(to_str(d.get("name", ""))), q) || str.lower(to_str(d.get("id", ""))) == q) { return d }
      i += 1
   }
   0
}

fn _resolve(any dev, bool input) any {
   if is_dict(dev) { return dev }
   if is_str(dev) { return find_device(dev, input) }
   0
}

fn _stream(str backend, str kind, any dev, any handle) dict {
   return {"backend": backend, "kind": kind, "device": is_dict(dev) ? dev.get("id", "") : "", "name": is_dict(dev) ? dev.get("name", "") : "", "handle": handle, "parser": parser(), "open": true}
}

fn open_input(any dev) any {
   "Opens an input device(dict from list_devices, or an id/name query).
   Returns a stream or 0."
   def d = _resolve(dev, true)
   if !is_dict(d) { return 0 }
   def b = to_str(d.get("backend", ""))
   if b == "winmm" {
      def h = mm_winmm.open_input(int(d.get("index", 0)))
      return is_dict(h) ? _stream(b, "in", d, h) : 0
   }
   if b == "coremidi" {
      def h = mm_coremidi.open_input(int(d.get("index", 0)))
      return is_dict(h) ? _stream(b, "in", d, h) : 0
   }
   if b != "alsa" { return 0 }
   def cl = _alsa_client()
   def port = seq.create_port(cl, "in " + to_str(d.get("name", "")), seq.PORT_INPUT)
   if port < 0 { return 0 }
   if !seq.connect_input(cl, port, int(d.get("client", 0)), int(d.get("port", 0))) {
      seq.delete_port(cl, port)
      return 0
   }
   _stream(b, "in", d, {"port": port, "client": int(d.get("client", 0)), "src_port": int(d.get("port", 0))})
}

fn open_output(any dev) any {
   "Opens an output device. Returns a stream or 0."
   def d = _resolve(dev, false)
   if !is_dict(d) { return 0 }
   def b = to_str(d.get("backend", ""))
   if b == "winmm" {
      def h = mm_winmm.open_output(int(d.get("index", 0)))
      return is_dict(h) ? _stream(b, "out", d, h) : 0
   }
   if b == "coremidi" {
      def h = mm_coremidi.open_output(int(d.get("index", 0)))
      return is_dict(h) ? _stream(b, "out", d, h) : 0
   }
   if b != "alsa" { return 0 }
   def cl = _alsa_client()
   def port = seq.create_port(cl, "out " + to_str(d.get("name", "")), seq.PORT_OUTPUT)
   if port < 0 { return 0 }
   if !seq.connect_output(cl, port, int(d.get("client", 0)), int(d.get("port", 0))) {
      seq.delete_port(cl, port)
      return 0
   }
   _stream(b, "out", d, {"port": port, "client": int(d.get("client", 0)), "dst_port": int(d.get("port", 0))})
}

fn open_virtual_input(str name) any {
   "Creates an input other programs can send to(ALSA port, CoreMIDI
   destination). WinMM has no virtual ports and returns 0."
   def b = backend_name()
   if b == "coremidi" {
      def h = mm_coremidi.open_virtual_input(name)
      return is_dict(h) ? _stream(b, "in", {"id": "virtual:" + name, "name": name}, h) : 0
   }
   if b != "alsa" { return 0 }
   def port = seq.create_port(_alsa_client(), name, seq.PORT_INPUT)
   port < 0 ? 0 : _stream(b, "in", {"id": "virtual:" + name, "name": name}, {"port": port, "virtual": true})
}

fn open_virtual_output(str name) any {
   "Creates an output other programs can connect to and receive from."
   def b = backend_name()
   if b == "coremidi" {
      def h = mm_coremidi.open_virtual_output(name)
      return is_dict(h) ? _stream(b, "out", {"id": "virtual:" + name, "name": name}, h) : 0
   }
   if b != "alsa" { return 0 }
   def port = seq.create_port(_alsa_client(), name, seq.PORT_OUTPUT)
   port < 0 ? 0 : _stream(b, "out", {"id": "virtual:" + name, "name": name}, {"port": port, "virtual": true})
}

fn _stamp(list msgs, str device) list {
   def now = __monotonic_ns_raw()
   mut i = 0
   while i < msgs.len {
      def any m = msgs.get(i, 0)
      if is_dict(m) {
         if int(m.get("time_ns", 0)) == 0 { m["time_ns"] = now }
         m["device"] = device
      }
      i += 1
   }
   msgs
}

fn read_messages(any stream, int timeout_ms=0, any filter=0) list {
   "Returns the messages received on an input stream, waiting up to
   `timeout_ms` (0 polls, negative waits). `filter` drops listed types."
   if !is_dict(stream) || !bool(stream.get("open", false)) || to_str(stream.get("kind", "")) != "in" { return [] }
   def b = to_str(stream.get("backend", ""))
   def dev = to_str(stream.get("device", ""))
   if b == "winmm" || b == "coremidi" {
      def chunks = b == "winmm" ? mm_winmm.read_bytes(stream.get("handle", 0), timeout_ms) : mm_coremidi.read_bytes(stream.get("handle", 0), timeout_ms)
      mut out = []
      mut i = 0
      while i < chunks.len {
         def msgs = parse(stream.get("parser", parser()), chunks.get(i, []))
         mut k = 0
         while k < msgs.len {
            def any m = msgs.get(k, 0)
            if is_dict(m) && !_filtered(filter, to_str(m.get("type", ""))) { out = out.append(m) }
            k += 1
         }
         i += 1
      }
      return _stamp(out, dev)
   }
   if b != "alsa" { return [] }
   ;; The shared client's fd carries every port: sort events into per-port
   ;; queues so several open inputs never steal each other's messages.
   def port = int(stream.get("handle", {}).get("port", -1))
   def key = to_str(port)
   if _alsa_pending.get(key, []).len == 0 {
      def got = seq.receive(_alsa_client(), timeout_ms, filter)
      mut i = 0
      while i < got.len {
         def any ev = got.get(i, 0)
         if is_dict(ev) {
            def pk = to_str(ev.get("port", -1))
            _alsa_pending[pk] = _alsa_pending.get(pk, []).append(ev)
         }
         i += 1
      }
   }
   def mine = _alsa_pending.get(key, [])
   _alsa_pending = _alsa_pending.delete(key)
   _stamp(mine, dev)
}

fn write_message(any stream, dict msg) bool {
   "Sends one message on an output stream."
   if !is_dict(stream) || !bool(stream.get("open", false)) || to_str(stream.get("kind", "")) != "out" { return false }
   def b = to_str(stream.get("backend", ""))
   if b == "winmm" { return mm_winmm.write_bytes(stream.get("handle", 0), encode(msg)) }
   if b == "coremidi" { return mm_coremidi.write_bytes(stream.get("handle", 0), encode(msg)) }
   if b != "alsa" { return false }
   seq.send(_alsa_client(), int(stream.get("handle", {}).get("port", -1)), msg)
}

fn close_stream(any stream) bool {
   "Closes a stream opened with open_input/open_output/open_virtual_*."
   if !is_dict(stream) || !bool(stream.get("open", false)) { return false }
   def b = to_str(stream.get("backend", ""))
   stream["open"] = false
   if b == "winmm" { return mm_winmm.close(stream.get("handle", 0)) }
   if b == "coremidi" { return mm_coremidi.close(stream.get("handle", 0)) }
   if b != "alsa" { return false }
   def h = stream.get("handle", {})
   def port = int(h.get("port", -1))
   def cl = _alsa_client()
   if !bool(h.get("virtual", false)) {
      if to_str(stream.get("kind", "")) == "in" {
         seq.disconnect_input(cl, port, int(h.get("client", 0)), int(h.get("src_port", 0)))
      } else {
         seq.disconnect_output(cl, port, int(h.get("client", 0)), int(h.get("dst_port", 0)))
      }
   }
   _alsa_pending = _alsa_pending.delete(to_str(port))
   seq.delete_port(cl, port)
}

;; ---------------------------------------------------------------------------
;; Raw MIDI device nodes (Linux): wire bytes through the codec.
fn raw_devices() list {
   "Lists raw MIDI device nodes(/dev/snd/midiC*D*) on Linux."
   seq.raw_devices()
}

fn raw_open(str path) any {
   "Opens a raw MIDI node for reading and writing. Returns a handle or 0."
   def h = seq.raw_open(path)
   if !is_dict(h) { return 0 }
   h["parser"] = parser()
   h
}

fn raw_close(any handle) bool {
   "Closes a raw MIDI handle."
   seq.raw_close(handle)
}

fn raw_send(any handle, dict msg) bool {
   "Encodes and writes one message to a raw MIDI handle."
   seq.raw_write_bytes(handle, encode(msg))
}

fn raw_receive(any handle, int timeout_ms=0) list {
   "Reads and parses pending messages from a raw MIDI handle."
   if !is_dict(handle) { return [] }
   def chunks = seq.raw_read_bytes(handle, timeout_ms)
   mut out = []
   mut i = 0
   while i < chunks.len {
      def msgs = parse(handle.get("parser", parser()), chunks.get(i, []))
      mut k = 0
      while k < msgs.len {
         def any m = msgs.get(k, 0)
         if is_dict(m) { out = out.append(m) }
         k += 1
      }
      i += 1
   }
   _stamp(out, to_str(handle.get("path", "")))
}

;; ---------------------------------------------------------------------------
;; Portable stream API

;; One shared sequencer client per process on Linux; every stream is a port.
;; Untyped dict globals: `mut any x = dict(8)` currently never holds the
;; dict (see docs/todo/ui.md compiler defects), and an int-initialised slot
;; mis-types a later dict, so both start as real dicts.
mut _alsa = dict(4)
mut _alsa_pending = dict(8)

fn backend_name() str {
   "Returns the MIDI backend for this OS: alsa, winmm, coremidi, or none."
   #linux { return "alsa" } #endif
   #windows { return "winmm" } #endif
   #macos { return "coremidi" } #endif
   "none"
}

fn _alsa_client() any {
   if !is_dict(_alsa) {
      def c = open_client("Nytrix")
      _alsa = c
   }
   _alsa
}

fn list_devices() list {
   "Lists MIDI devices on every OS as dicts {id, name, input, output,
    backend}. `input` devices can be opened with `open_input` (keyboards,
    controllers); `output` devices with `open_output` (synths, interfaces)."
   def b = backend_name()
   if b == "winmm" { return mm_winmm.devices() }
   if b == "coremidi" { return mm_coremidi.devices() }
   if b != "alsa" { return [] }
   def cl = _alsa_client()
   def own = client_id(cl)
   def raw = devices(cl)
   mut out = []
   mut i = 0
   while i < raw.len {
      def d = raw[i]
      def c = int(d.get("client", -1))
      ;; Skip the kernel System client (timer/announce) and our own ports.
      if c != 0 && c != own && (bool(d.get("input", false)) || bool(d.get("output", false))) {
         out = out.append({
            "id": "alsa:" + to_str(c) + ":" + to_str(d.get("port", 0)),
            "name": to_str(d.get("client_name", "")) + ": " + to_str(d.get("name", "")),
            "input": bool(d.get("input", false)),
            "output": bool(d.get("output", false)),
            "backend": "alsa",
            "client": c,
            "port": int(d.get("port", 0))
         })
      }
      i += 1
   }
   out
}

fn find_device(str query, bool input=true) any {
   "Returns the first device whose id or name contains `query` (case
    insensitive) and supports the direction, or 0."
   def q = str.lower(query)
   def devs = list_devices()
   mut i = 0
   while i < devs.len {
      def d = devs[i]
      def dir_ok = input ? bool(d.get("input", false)) : bool(d.get("output", false))
      if dir_ok && (q.len == 0 || str.str_contains(str.lower(to_str(d.get("name", ""))), q) || str.lower(to_str(d.get("id", ""))) == q) { return d }
      i += 1
   }
   0
}

fn _resolve(any dev, bool input) any {
   if is_dict(dev) { return dev }
   if is_str(dev) { return find_device(dev, input) }
   0
}

fn _stream(str backend, str kind, any dev, any handle) dict {
   {"backend": backend, "kind": kind, "device": is_dict(dev) ? dev.get("id", "") : "", "name": is_dict(dev) ? dev.get("name", "") : "", "handle": handle, "parser": parser(), "open": true}
}

fn open_input(any dev) any {
   "Opens an input device (dict from list_devices, or an id/name query).
    Returns a stream or 0."
   def d = _resolve(dev, true)
   if !is_dict(d) { return 0 }
   def b = to_str(d.get("backend", ""))
   if b == "winmm" {
      def h = mm_winmm.open_input(int(d.get("index", 0)))
      return is_dict(h) ? _stream(b, "in", d, h) : 0
   }
   if b == "coremidi" {
      def h = mm_coremidi.open_input(int(d.get("index", 0)))
      return is_dict(h) ? _stream(b, "in", d, h) : 0
   }
   if b != "alsa" { return 0 }
   def cl = _alsa_client()
   def port = create_port(cl, "in " + to_str(d.get("name", "")), PORT_INPUT)
   if port < 0 { return 0 }
   if !connect_input(cl, port, int(d.get("client", 0)), int(d.get("port", 0))) {
      delete_port(cl, port)
      return 0
   }
   _stream(b, "in", d, {"port": port, "client": int(d.get("client", 0)), "src_port": int(d.get("port", 0))})
}

fn open_output(any dev) any {
   "Opens an output device. Returns a stream or 0."
   def d = _resolve(dev, false)
   if !is_dict(d) { return 0 }
   def b = to_str(d.get("backend", ""))
   if b == "winmm" {
      def h = mm_winmm.open_output(int(d.get("index", 0)))
      return is_dict(h) ? _stream(b, "out", d, h) : 0
   }
   if b == "coremidi" {
      def h = mm_coremidi.open_output(int(d.get("index", 0)))
      return is_dict(h) ? _stream(b, "out", d, h) : 0
   }
   if b != "alsa" { return 0 }
   def cl = _alsa_client()
   def port = create_port(cl, "out " + to_str(d.get("name", "")), PORT_OUTPUT)
   if port < 0 { return 0 }
   if !connect_output(cl, port, int(d.get("client", 0)), int(d.get("port", 0))) {
      delete_port(cl, port)
      return 0
   }
   _stream(b, "out", d, {"port": port, "client": int(d.get("client", 0)), "dst_port": int(d.get("port", 0))})
}

fn open_virtual_input(str name) any {
   "Creates an input other programs can send to (ALSA port, CoreMIDI
    destination). WinMM has no virtual ports and returns 0."
   def b = backend_name()
   if b == "coremidi" {
      def h = mm_coremidi.open_virtual_input(name)
      return is_dict(h) ? _stream(b, "in", {"id": "virtual:" + name, "name": name}, h) : 0
   }
   if b != "alsa" { return 0 }
   def port = create_port(_alsa_client(), name, PORT_INPUT)
   port < 0 ? 0 : _stream(b, "in", {"id": "virtual:" + name, "name": name}, {"port": port, "virtual": true})
}

fn open_virtual_output(str name) any {
   "Creates an output other programs can connect to and receive from."
   def b = backend_name()
   if b == "coremidi" {
      def h = mm_coremidi.open_virtual_output(name)
      return is_dict(h) ? _stream(b, "out", {"id": "virtual:" + name, "name": name}, h) : 0
   }
   if b != "alsa" { return 0 }
   def port = create_port(_alsa_client(), name, PORT_OUTPUT)
   port < 0 ? 0 : _stream(b, "out", {"id": "virtual:" + name, "name": name}, {"port": port, "virtual": true})
}

fn _stamp(list msgs, str device) list {
   def now = __monotonic_ns_raw()
   mut i = 0
   while i < msgs.len {
      def any m = msgs.get(i, 0)
      if is_dict(m) {
         if int(m.get("time_ns", 0)) == 0 { m["time_ns"] = now }
         m["device"] = device
      }
      i += 1
   }
   msgs
}

fn read_messages(any stream, int timeout_ms=0, any filter=0) list {
   "Returns the messages received on an input stream, waiting up to
    `timeout_ms` (0 polls, negative waits). `filter` drops listed types."
   if !is_dict(stream) || !bool(stream.get("open", false)) || to_str(stream.get("kind", "")) != "in" { return [] }
   def b = to_str(stream.get("backend", ""))
   def dev = to_str(stream.get("device", ""))
   if b == "winmm" || b == "coremidi" {
      def chunks = b == "winmm" ? mm_winmm.read_bytes(stream.get("handle", 0), timeout_ms) : mm_coremidi.read_bytes(stream.get("handle", 0), timeout_ms)
      mut out = []
      mut i = 0
      while i < chunks.len {
         def msgs = parse(stream.get("parser", parser()), chunks[i])
         mut k = 0
         while k < msgs.len {
            if !_filtered(filter, to_str(msgs[k].get("type", ""))) { out = out.append(msgs[k]) }
            k += 1
         }
         i += 1
      }
      return _stamp(out, dev)
   }
   if b != "alsa" { return [] }
   ;; The shared client's fd carries every port: sort events into per-port
   ;; queues so several open inputs never steal each other's messages.
   def port = int(stream.get("handle", {}).get("port", -1))
   def key = to_str(port)
   if _alsa_pending.get(key, []).len == 0 {
      def got = receive(_alsa_client(), timeout_ms, filter)
      mut i = 0
      while i < got.len {
         def pk = to_str(got[i].get("port", -1))
         _alsa_pending[pk] = _alsa_pending.get(pk, []).append(got[i])
         i += 1
      }
   }
   def mine = _alsa_pending.get(key, [])
   _alsa_pending = _alsa_pending.delete(key)
   _stamp(mine, dev)
}

fn write_message(any stream, dict msg) bool {
   "Sends one message on an output stream."
   if !is_dict(stream) || !bool(stream.get("open", false)) || to_str(stream.get("kind", "")) != "out" { return false }
   def b = to_str(stream.get("backend", ""))
   if b == "winmm" { return mm_winmm.write_bytes(stream.get("handle", 0), encode(msg)) }
   if b == "coremidi" { return mm_coremidi.write_bytes(stream.get("handle", 0), encode(msg)) }
   if b != "alsa" { return false }
   send(_alsa_client(), int(stream.get("handle", {}).get("port", -1)), msg)
}

fn close_stream(any stream) bool {
   "Closes a stream opened with open_input/open_output/open_virtual_*."
   if !is_dict(stream) || !bool(stream.get("open", false)) { return false }
   def b = to_str(stream.get("backend", ""))
   stream["open"] = false
   if b == "winmm" { return mm_winmm.close(stream.get("handle", 0)) }
   if b == "coremidi" { return mm_coremidi.close(stream.get("handle", 0)) }
   if b != "alsa" { return false }
   def h = stream.get("handle", {})
   def port = int(h.get("port", -1))
   def cl = _alsa_client()
   if !bool(h.get("virtual", false)) {
      if to_str(stream.get("kind", "")) == "in" {
         disconnect_input(cl, port, int(h.get("client", 0)), int(h.get("src_port", 0)))
      } else {
         disconnect_output(cl, port, int(h.get("client", 0)), int(h.get("dst_port", 0)))
      }
   }
   _alsa_pending = _alsa_pending.delete(to_str(port))
   delete_port(cl, port)
}

;; ---------------------------------------------------------------------------
;; Portable stream API

;; One shared sequencer client per process on Linux; every stream is a port.
;; Untyped dict globals: `mut any x = dict(8)` currently never holds the
;; dict (see docs/todo/ui.md compiler defects), and an int-initialised slot
;; mis-types a later dict, so both start as real dicts.
mut _alsa = dict(4)
mut _alsa_pending = dict(8)

fn backend_name() str {
   "Returns the MIDI backend for this OS: alsa, winmm, coremidi, or none."
   #linux { return "alsa" } #endif
   #windows { return "winmm" } #endif
   #macos { return "coremidi" } #endif
   "none"
}

fn _alsa_client() any {
   if !is_dict(_alsa) {
      def c = open_client("Nytrix")
      _alsa = c
   }
   _alsa
}

fn list_devices() list {
   "Lists MIDI devices on every OS as dicts {id, name, input, output,
    backend}. `input` devices can be opened with `open_input` (keyboards,
    controllers); `output` devices with `open_output` (synths, interfaces)."
   def b = backend_name()
   if b == "winmm" { return mm_winmm.devices() }
   if b == "coremidi" { return mm_coremidi.devices() }
   if b != "alsa" { return [] }
   def cl = _alsa_client()
   def own = client_id(cl)
   def raw = devices(cl)
   mut out = []
   mut i = 0
   while i < raw.len {
      def d = raw[i]
      def c = int(d.get("client", -1))
      ;; Skip the kernel System client (timer/announce) and our own ports.
      if c != 0 && c != own && (bool(d.get("input", false)) || bool(d.get("output", false))) {
         out = out.append({
            "id": "alsa:" + to_str(c) + ":" + to_str(d.get("port", 0)),
            "name": to_str(d.get("client_name", "")) + ": " + to_str(d.get("name", "")),
            "input": bool(d.get("input", false)),
            "output": bool(d.get("output", false)),
            "backend": "alsa",
            "client": c,
            "port": int(d.get("port", 0))
         })
      }
      i += 1
   }
   out
}

fn find_device(str query, bool input=true) any {
   "Returns the first device whose id or name contains `query` (case
    insensitive) and supports the direction, or 0."
   def q = str.lower(query)
   def devs = list_devices()
   mut i = 0
   while i < devs.len {
      def d = devs[i]
      def dir_ok = input ? bool(d.get("input", false)) : bool(d.get("output", false))
      if dir_ok && (q.len == 0 || str.str_contains(str.lower(to_str(d.get("name", ""))), q) || str.lower(to_str(d.get("id", ""))) == q) { return d }
      i += 1
   }
   0
}

fn _resolve(any dev, bool input) any {
   if is_dict(dev) { return dev }
   if is_str(dev) { return find_device(dev, input) }
   0
}

fn _stream(str backend, str kind, any dev, any handle) dict {
   {"backend": backend, "kind": kind, "device": is_dict(dev) ? dev.get("id", "") : "", "name": is_dict(dev) ? dev.get("name", "") : "", "handle": handle, "parser": parser(), "open": true}
}

fn open_input(any dev) any {
   "Opens an input device (dict from list_devices, or an id/name query).
    Returns a stream or 0."
   def d = _resolve(dev, true)
   if !is_dict(d) { return 0 }
   def b = to_str(d.get("backend", ""))
   if b == "winmm" {
      def h = mm_winmm.open_input(int(d.get("index", 0)))
      return is_dict(h) ? _stream(b, "in", d, h) : 0
   }
   if b == "coremidi" {
      def h = mm_coremidi.open_input(int(d.get("index", 0)))
      return is_dict(h) ? _stream(b, "in", d, h) : 0
   }
   if b != "alsa" { return 0 }
   def cl = _alsa_client()
   def port = create_port(cl, "in " + to_str(d.get("name", "")), PORT_INPUT)
   if port < 0 { return 0 }
   if !connect_input(cl, port, int(d.get("client", 0)), int(d.get("port", 0))) {
      delete_port(cl, port)
      return 0
   }
   _stream(b, "in", d, {"port": port, "client": int(d.get("client", 0)), "src_port": int(d.get("port", 0))})
}

fn open_output(any dev) any {
   "Opens an output device. Returns a stream or 0."
   def d = _resolve(dev, false)
   if !is_dict(d) { return 0 }
   def b = to_str(d.get("backend", ""))
   if b == "winmm" {
      def h = mm_winmm.open_output(int(d.get("index", 0)))
      return is_dict(h) ? _stream(b, "out", d, h) : 0
   }
   if b == "coremidi" {
      def h = mm_coremidi.open_output(int(d.get("index", 0)))
      return is_dict(h) ? _stream(b, "out", d, h) : 0
   }
   if b != "alsa" { return 0 }
   def cl = _alsa_client()
   def port = create_port(cl, "out " + to_str(d.get("name", "")), PORT_OUTPUT)
   if port < 0 { return 0 }
   if !connect_output(cl, port, int(d.get("client", 0)), int(d.get("port", 0))) {
      delete_port(cl, port)
      return 0
   }
   _stream(b, "out", d, {"port": port, "client": int(d.get("client", 0)), "dst_port": int(d.get("port", 0))})
}

fn open_virtual_input(str name) any {
   "Creates an input other programs can send to (ALSA port, CoreMIDI
    destination). WinMM has no virtual ports and returns 0."
   def b = backend_name()
   if b == "coremidi" {
      def h = mm_coremidi.open_virtual_input(name)
      return is_dict(h) ? _stream(b, "in", {"id": "virtual:" + name, "name": name}, h) : 0
   }
   if b != "alsa" { return 0 }
   def port = create_port(_alsa_client(), name, PORT_INPUT)
   port < 0 ? 0 : _stream(b, "in", {"id": "virtual:" + name, "name": name}, {"port": port, "virtual": true})
}

fn open_virtual_output(str name) any {
   "Creates an output other programs can connect to and receive from."
   def b = backend_name()
   if b == "coremidi" {
      def h = mm_coremidi.open_virtual_output(name)
      return is_dict(h) ? _stream(b, "out", {"id": "virtual:" + name, "name": name}, h) : 0
   }
   if b != "alsa" { return 0 }
   def port = create_port(_alsa_client(), name, PORT_OUTPUT)
   port < 0 ? 0 : _stream(b, "out", {"id": "virtual:" + name, "name": name}, {"port": port, "virtual": true})
}

fn _stamp(list msgs, str device) list {
   def now = __monotonic_ns_raw()
   mut i = 0
   while i < msgs.len {
      def any m = msgs.get(i, 0)
      if is_dict(m) {
         if int(m.get("time_ns", 0)) == 0 { m["time_ns"] = now }
         m["device"] = device
      }
      i += 1
   }
   msgs
}

fn read_messages(any stream, int timeout_ms=0, any filter=0) list {
   "Returns the messages received on an input stream, waiting up to
    `timeout_ms` (0 polls, negative waits). `filter` drops listed types."
   if !is_dict(stream) || !bool(stream.get("open", false)) || to_str(stream.get("kind", "")) != "in" { return [] }
   def b = to_str(stream.get("backend", ""))
   def dev = to_str(stream.get("device", ""))
   if b == "winmm" || b == "coremidi" {
      def chunks = b == "winmm" ? mm_winmm.read_bytes(stream.get("handle", 0), timeout_ms) : mm_coremidi.read_bytes(stream.get("handle", 0), timeout_ms)
      mut out = []
      mut i = 0
      while i < chunks.len {
         def msgs = parse(stream.get("parser", parser()), chunks[i])
         mut k = 0
         while k < msgs.len {
            if !_filtered(filter, to_str(msgs[k].get("type", ""))) { out = out.append(msgs[k]) }
            k += 1
         }
         i += 1
      }
      return _stamp(out, dev)
   }
   if b != "alsa" { return [] }
   ;; The shared client's fd carries every port: sort events into per-port
   ;; queues so several open inputs never steal each other's messages.
   def port = int(stream.get("handle", {}).get("port", -1))
   def key = to_str(port)
   if _alsa_pending.get(key, []).len == 0 {
      def got = receive(_alsa_client(), timeout_ms, filter)
      mut i = 0
      while i < got.len {
         def pk = to_str(got[i].get("port", -1))
         _alsa_pending[pk] = _alsa_pending.get(pk, []).append(got[i])
         i += 1
      }
   }
   def mine = _alsa_pending.get(key, [])
   _alsa_pending = _alsa_pending.delete(key)
   _stamp(mine, dev)
}

fn write_message(any stream, dict msg) bool {
   "Sends one message on an output stream."
   if !is_dict(stream) || !bool(stream.get("open", false)) || to_str(stream.get("kind", "")) != "out" { return false }
   def b = to_str(stream.get("backend", ""))
   if b == "winmm" { return mm_winmm.write_bytes(stream.get("handle", 0), encode(msg)) }
   if b == "coremidi" { return mm_coremidi.write_bytes(stream.get("handle", 0), encode(msg)) }
   if b != "alsa" { return false }
   send(_alsa_client(), int(stream.get("handle", {}).get("port", -1)), msg)
}

fn close_stream(any stream) bool {
   "Closes a stream opened with open_input/open_output/open_virtual_*."
   if !is_dict(stream) || !bool(stream.get("open", false)) { return false }
   def b = to_str(stream.get("backend", ""))
   stream["open"] = false
   if b == "winmm" { return mm_winmm.close(stream.get("handle", 0)) }
   if b == "coremidi" { return mm_coremidi.close(stream.get("handle", 0)) }
   if b != "alsa" { return false }
   def h = stream.get("handle", {})
   def port = int(h.get("port", -1))
   def cl = _alsa_client()
   if !bool(h.get("virtual", false)) {
      if to_str(stream.get("kind", "")) == "in" {
         disconnect_input(cl, port, int(h.get("client", 0)), int(h.get("src_port", 0)))
      } else {
         disconnect_output(cl, port, int(h.get("client", 0)), int(h.get("dst_port", 0)))
      }
   }
   _alsa_pending = _alsa_pending.delete(to_str(port))
   delete_port(cl, port)
}

;; ---------------------------------------------------------------------------
;; Portable stream API

;; One shared sequencer client per process on Linux; every stream is a port.
;; Untyped dict globals: `mut any x = dict(8)` currently never holds the
;; dict (see docs/todo/ui.md compiler defects), and an int-initialised slot
;; mis-types a later dict, so both start as real dicts.
mut _alsa = dict(4)
mut _alsa_pending = dict(8)

fn backend_name() str {
   "Returns the MIDI backend for this OS: alsa, winmm, coremidi, or none."
   #linux { return "alsa" } #endif
   #windows { return "winmm" } #endif
   #macos { return "coremidi" } #endif
   "none"
}

fn _alsa_client() any {
   if !is_dict(_alsa) {
      def c = open_client("Nytrix")
      _alsa = c
   }
   _alsa
}

fn list_devices() list {
   "Lists MIDI devices on every OS as dicts {id, name, input, output,
    backend}. `input` devices can be opened with `open_input` (keyboards,
    controllers); `output` devices with `open_output` (synths, interfaces)."
   def b = backend_name()
   if b == "winmm" { return mm_winmm.devices() }
   if b == "coremidi" { return mm_coremidi.devices() }
   if b != "alsa" { return [] }
   def cl = _alsa_client()
   def own = client_id(cl)
   def raw = devices(cl)
   mut out = []
   mut i = 0
   while i < raw.len {
      def d = raw[i]
      def c = int(d.get("client", -1))
      ;; Skip the kernel System client (timer/announce) and our own ports.
      if c != 0 && c != own && (bool(d.get("input", false)) || bool(d.get("output", false))) {
         out = out.append({
            "id": "alsa:" + to_str(c) + ":" + to_str(d.get("port", 0)),
            "name": to_str(d.get("client_name", "")) + ": " + to_str(d.get("name", "")),
            "input": bool(d.get("input", false)),
            "output": bool(d.get("output", false)),
            "backend": "alsa",
            "client": c,
            "port": int(d.get("port", 0))
         })
      }
      i += 1
   }
   out
}

fn find_device(str query, bool input=true) any {
   "Returns the first device whose id or name contains `query` (case
    insensitive) and supports the direction, or 0."
   def q = str.lower(query)
   def devs = list_devices()
   mut i = 0
   while i < devs.len {
      def d = devs[i]
      def dir_ok = input ? bool(d.get("input", false)) : bool(d.get("output", false))
      if dir_ok && (q.len == 0 || str.str_contains(str.lower(to_str(d.get("name", ""))), q) || str.lower(to_str(d.get("id", ""))) == q) { return d }
      i += 1
   }
   0
}

fn _resolve(any dev, bool input) any {
   if is_dict(dev) { return dev }
   if is_str(dev) { return find_device(dev, input) }
   0
}

fn _stream(str backend, str kind, any dev, any handle) dict {
   {"backend": backend, "kind": kind, "device": is_dict(dev) ? dev.get("id", "") : "", "name": is_dict(dev) ? dev.get("name", "") : "", "handle": handle, "parser": parser(), "open": true}
}

fn open_input(any dev) any {
   "Opens an input device (dict from list_devices, or an id/name query).
    Returns a stream or 0."
   def d = _resolve(dev, true)
   if !is_dict(d) { return 0 }
   def b = to_str(d.get("backend", ""))
   if b == "winmm" {
      def h = mm_winmm.open_input(int(d.get("index", 0)))
      return is_dict(h) ? _stream(b, "in", d, h) : 0
   }
   if b == "coremidi" {
      def h = mm_coremidi.open_input(int(d.get("index", 0)))
      return is_dict(h) ? _stream(b, "in", d, h) : 0
   }
   if b != "alsa" { return 0 }
   def cl = _alsa_client()
   def port = create_port(cl, "in " + to_str(d.get("name", "")), PORT_INPUT)
   if port < 0 { return 0 }
   if !connect_input(cl, port, int(d.get("client", 0)), int(d.get("port", 0))) {
      delete_port(cl, port)
      return 0
   }
   _stream(b, "in", d, {"port": port, "client": int(d.get("client", 0)), "src_port": int(d.get("port", 0))})
}

fn open_output(any dev) any {
   "Opens an output device. Returns a stream or 0."
   def d = _resolve(dev, false)
   if !is_dict(d) { return 0 }
   def b = to_str(d.get("backend", ""))
   if b == "winmm" {
      def h = mm_winmm.open_output(int(d.get("index", 0)))
      return is_dict(h) ? _stream(b, "out", d, h) : 0
   }
   if b == "coremidi" {
      def h = mm_coremidi.open_output(int(d.get("index", 0)))
      return is_dict(h) ? _stream(b, "out", d, h) : 0
   }
   if b != "alsa" { return 0 }
   def cl = _alsa_client()
   def port = create_port(cl, "out " + to_str(d.get("name", "")), PORT_OUTPUT)
   if port < 0 { return 0 }
   if !connect_output(cl, port, int(d.get("client", 0)), int(d.get("port", 0))) {
      delete_port(cl, port)
      return 0
   }
   _stream(b, "out", d, {"port": port, "client": int(d.get("client", 0)), "dst_port": int(d.get("port", 0))})
}

fn open_virtual_input(str name) any {
   "Creates an input other programs can send to (ALSA port, CoreMIDI
    destination). WinMM has no virtual ports and returns 0."
   def b = backend_name()
   if b == "coremidi" {
      def h = mm_coremidi.open_virtual_input(name)
      return is_dict(h) ? _stream(b, "in", {"id": "virtual:" + name, "name": name}, h) : 0
   }
   if b != "alsa" { return 0 }
   def port = create_port(_alsa_client(), name, PORT_INPUT)
   port < 0 ? 0 : _stream(b, "in", {"id": "virtual:" + name, "name": name}, {"port": port, "virtual": true})
}

fn open_virtual_output(str name) any {
   "Creates an output other programs can connect to and receive from."
   def b = backend_name()
   if b == "coremidi" {
      def h = mm_coremidi.open_virtual_output(name)
      return is_dict(h) ? _stream(b, "out", {"id": "virtual:" + name, "name": name}, h) : 0
   }
   if b != "alsa" { return 0 }
   def port = create_port(_alsa_client(), name, PORT_OUTPUT)
   port < 0 ? 0 : _stream(b, "out", {"id": "virtual:" + name, "name": name}, {"port": port, "virtual": true})
}

fn _stamp(list msgs, str device) list {
   def now = __monotonic_ns_raw()
   mut i = 0
   while i < msgs.len {
      def any m = msgs.get(i, 0)
      if is_dict(m) {
         if int(m.get("time_ns", 0)) == 0 { m["time_ns"] = now }
         m["device"] = device
      }
      i += 1
   }
   msgs
}

fn read_messages(any stream, int timeout_ms=0, any filter=0) list {
   "Returns the messages received on an input stream, waiting up to
    `timeout_ms` (0 polls, negative waits). `filter` drops listed types."
   if !is_dict(stream) || !bool(stream.get("open", false)) || to_str(stream.get("kind", "")) != "in" { return [] }
   def b = to_str(stream.get("backend", ""))
   def dev = to_str(stream.get("device", ""))
   if b == "winmm" || b == "coremidi" {
      def chunks = b == "winmm" ? mm_winmm.read_bytes(stream.get("handle", 0), timeout_ms) : mm_coremidi.read_bytes(stream.get("handle", 0), timeout_ms)
      mut out = []
      mut i = 0
      while i < chunks.len {
         def msgs = parse(stream.get("parser", parser()), chunks[i])
         mut k = 0
         while k < msgs.len {
            if !_filtered(filter, to_str(msgs[k].get("type", ""))) { out = out.append(msgs[k]) }
            k += 1
         }
         i += 1
      }
      return _stamp(out, dev)
   }
   if b != "alsa" { return [] }
   ;; The shared client's fd carries every port: sort events into per-port
   ;; queues so several open inputs never steal each other's messages.
   def port = int(stream.get("handle", {}).get("port", -1))
   def key = to_str(port)
   if _alsa_pending.get(key, []).len == 0 {
      def got = receive(_alsa_client(), timeout_ms, filter)
      mut i = 0
      while i < got.len {
         def pk = to_str(got[i].get("port", -1))
         _alsa_pending[pk] = _alsa_pending.get(pk, []).append(got[i])
         i += 1
      }
   }
   def mine = _alsa_pending.get(key, [])
   _alsa_pending = _alsa_pending.delete(key)
   _stamp(mine, dev)
}

fn write_message(any stream, dict msg) bool {
   "Sends one message on an output stream."
   if !is_dict(stream) || !bool(stream.get("open", false)) || to_str(stream.get("kind", "")) != "out" { return false }
   def b = to_str(stream.get("backend", ""))
   if b == "winmm" { return mm_winmm.write_bytes(stream.get("handle", 0), encode(msg)) }
   if b == "coremidi" { return mm_coremidi.write_bytes(stream.get("handle", 0), encode(msg)) }
   if b != "alsa" { return false }
   send(_alsa_client(), int(stream.get("handle", {}).get("port", -1)), msg)
}

fn close_stream(any stream) bool {
   "Closes a stream opened with open_input/open_output/open_virtual_*."
   if !is_dict(stream) || !bool(stream.get("open", false)) { return false }
   def b = to_str(stream.get("backend", ""))
   stream["open"] = false
   if b == "winmm" { return mm_winmm.close(stream.get("handle", 0)) }
   if b == "coremidi" { return mm_coremidi.close(stream.get("handle", 0)) }
   if b != "alsa" { return false }
   def h = stream.get("handle", {})
   def port = int(h.get("port", -1))
   def cl = _alsa_client()
   if !bool(h.get("virtual", false)) {
      if to_str(stream.get("kind", "")) == "in" {
         disconnect_input(cl, port, int(h.get("client", 0)), int(h.get("src_port", 0)))
      } else {
         disconnect_output(cl, port, int(h.get("client", 0)), int(h.get("dst_port", 0)))
      }
   }
   _alsa_pending = _alsa_pending.delete(to_str(port))
   delete_port(cl, port)
}
