;; Keywords: midi winmm windows sound os
;; Windows MIDI backend over WinMM (midiIn*/midiOut*).
;;
;; Input uses CALLBACK_THREAD: WinMM posts MM_MIM_DATA / MM_MIM_LONGDATA to
;; the opening thread's message queue and `read` drains them with
;; PeekMessage, so no Ny code ever runs on a driver thread. Sysex input uses
;; a ring of prepared MIDIHDR buffers that are re-queued after each delivery.
;; WinMM has no virtual ports; `open_virtual` reports that explicitly.
;; References:
;; - core.os.sound.midi
;; - learn.microsoft.com/windows/win32/multimedia/midi-functions
module core.os.sound.midi.winmm(available, devices, open_input, open_output, read_bytes, write_bytes, close)
use core
use core.mem
use core.str as str

#windows {
   #link "winmm"
   #link "user32"
   #link "kernel32"
   #include <windows.h>
   #include <mmsystem.h>
   extern "" {
      fn midiInGetNumDevs() u32
      fn midiOutGetNumDevs() u32
      fn midiInGetDevCapsA(u64 id, ptr caps, u32 size) u32
      fn midiOutGetDevCapsA(u64 id, ptr caps, u32 size) u32
      fn midiInOpen(ptr phmi, u32 id, u64 callback, u64 instance, u32 flags) u32
      fn midiInStart(ptr hmi) u32
      fn midiInStop(ptr hmi) u32
      fn midiInReset(ptr hmi) u32
      fn midiInClose(ptr hmi) u32
      fn midiInPrepareHeader(ptr hmi, ptr hdr, u32 size) u32
      fn midiInUnprepareHeader(ptr hmi, ptr hdr, u32 size) u32
      fn midiInAddBuffer(ptr hmi, ptr hdr, u32 size) u32
      fn midiOutOpen(ptr phmo, u32 id, u64 callback, u64 instance, u32 flags) u32
      fn midiOutShortMsg(ptr hmo, u32 msg) u32
      fn midiOutPrepareHeader(ptr hmo, ptr hdr, u32 size) u32
      fn midiOutUnprepareHeader(ptr hmo, ptr hdr, u32 size) u32
      fn midiOutLongMsg(ptr hmo, ptr hdr, u32 size) u32
      fn midiOutReset(ptr hmo) u32
      fn midiOutClose(ptr hmo) u32
      fn GetCurrentThreadId() u32
      fn PeekMessageA(ptr msg, ptr hwnd, u32 min_msg, u32 max_msg, u32 remove) i32
      fn MsgWaitForMultipleObjects(u32 count, ptr handles, i32 wait_all, u32 ms, u32 mask) u32
      fn Sleep(u32 ms) any
   }
} #else {
   fn midiInGetNumDevs() int { 0 }
   fn midiOutGetNumDevs() int { 0 }
   fn midiInGetDevCapsA(any _id, any _caps, any _size) int { 1 }
   fn midiOutGetDevCapsA(any _id, any _caps, any _size) int { 1 }
   fn midiInOpen(any _p, any _id, any _cb, any _inst, any _flags) int { 1 }
   fn midiInStart(any _h) int { 1 }
   fn midiInStop(any _h) int { 1 }
   fn midiInReset(any _h) int { 1 }
   fn midiInClose(any _h) int { 1 }
   fn midiInPrepareHeader(any _h, any _hdr, any _size) int { 1 }
   fn midiInUnprepareHeader(any _h, any _hdr, any _size) int { 1 }
   fn midiInAddBuffer(any _h, any _hdr, any _size) int { 1 }
   fn midiOutOpen(any _p, any _id, any _cb, any _inst, any _flags) int { 1 }
   fn midiOutShortMsg(any _h, any _msg) int { 1 }
   fn midiOutPrepareHeader(any _h, any _hdr, any _size) int { 1 }
   fn midiOutUnprepareHeader(any _h, any _hdr, any _size) int { 1 }
   fn midiOutLongMsg(any _h, any _hdr, any _size) int { 1 }
   fn midiOutReset(any _h) int { 1 }
   fn midiOutClose(any _h) int { 1 }
   fn GetCurrentThreadId() int { 0 }
   fn PeekMessageA(any _m, any _w, any _a, any _b, any _r) int { 0 }
   fn MsgWaitForMultipleObjects(any _c, any _h, any _w, any _ms, any _mask) int { 0 }
   fn Sleep(any _ms) any { 0 }
} #endif
def _MMSYSERR_NOERROR = 0
def _CALLBACK_NULL = 0
def _CALLBACK_THREAD = 0x20000
def _MIDI_IO_STATUS = 0x20
def _MM_MIM_DATA = 0x3C3
def _MM_MIM_LONGDATA = 0x3C4
def _MM_MIM_FIRST = 0x3C1
def _MM_MIM_LAST = 0x3CC
def _PM_REMOVE = 1
def _QS_POSTMESSAGE = 0x8
def _MSG_SIZE = 48
def _MSG_MESSAGE = 8
def _MSG_WPARAM = 16
def _MSG_LPARAM = 24

;; MIDIHDR on LP64 Windows: lpData(8) len(4) recorded(4) user(8) flags(4)
;; pad(4) lpNext(8) reserved(8) offset(4) pad(4) dwReserved[8](64) = 120.
def _HDR_SIZE = 120
def _HDR_DATA = 0
def _HDR_LEN = 8
def _HDR_RECORDED = 12
def _HDR_FLAGS = 24
def _MHDR_DONE = 1
def _SYSEX_BUFS = 4
def _SYSEX_BUF_LEN = 4096
def _IN_CAPS_SIZE = 44
def _OUT_CAPS_SIZE = 52
def _CAPS_NAME = 8

fn available() bool {
   "Reports whether WinMM MIDI is present."
   #windows { true } #else { false } #endif
}

fn devices() list {
   "Lists WinMM inputs and outputs as {id, name, input, output, backend}."
   mut out = []
   def in_n = int(midiInGetNumDevs())
   def caps = zalloc(64)
   mut i = 0
   while i < in_n {
      memset(caps, 0, 64)
      if int(midiInGetDevCapsA(i, caps, _IN_CAPS_SIZE)) == _MMSYSERR_NOERROR {
         store8(caps, 0, _CAPS_NAME + 31)
         out = out.append({"id": "winmm:in:" + to_str(i), "index": i, "name": str.cstr_to_str(caps, _CAPS_NAME), "input": true, "output": false, "backend": "winmm"})
      }
      i += 1
   }
   def out_n = int(midiOutGetNumDevs())
   i = 0
   while i < out_n {
      memset(caps, 0, 64)
      if int(midiOutGetDevCapsA(i, caps, _OUT_CAPS_SIZE)) == _MMSYSERR_NOERROR {
         store8(caps, 0, _CAPS_NAME + 31)
         out = out.append({"id": "winmm:out:" + to_str(i), "index": i, "name": str.cstr_to_str(caps, _CAPS_NAME), "input": false, "output": true, "backend": "winmm"})
      }
      i += 1
   }
   free(caps)
   out
}

fn _queue_sysex_buffer(any h, any hdr) bool {
   int(midiInAddBuffer(h, hdr, _HDR_SIZE)) == _MMSYSERR_NOERROR
}

fn open_input(int index) any {
   "Opens WinMM input `index` and starts it. Returns a stream dict or 0."
   def msg = zalloc(_MSG_SIZE)
   ;; Make sure this thread owns a message queue before WinMM posts to it.
   def ignored = PeekMessageA(msg, 0, 0, 0, 0)
   def hp = zalloc(8)
   def rc = int(midiInOpen(hp, index, int(GetCurrentThreadId()), 0, _CALLBACK_THREAD))
   if rc != _MMSYSERR_NOERROR {
      free(hp)
      free(msg)
      return 0
   }
   def h = load64(hp, 0)
   free(hp)
   mut hdrs = []
   mut k = 0
   while k < _SYSEX_BUFS {
      def hdr = zalloc(_HDR_SIZE)
      def data = zalloc(_SYSEX_BUF_LEN)
      store64(hdr, data, _HDR_DATA)
      store32(hdr, _SYSEX_BUF_LEN, _HDR_LEN)
      if int(midiInPrepareHeader(h, hdr, _HDR_SIZE)) == _MMSYSERR_NOERROR && _queue_sysex_buffer(h, hdr) {
         hdrs = hdrs.append(hdr)
      }
      k += 1
   }
   midiInStart(h)
   {"kind": "in", "handle": h, "msg": msg, "hdrs": hdrs, "open": true, "index": index, "ignored": ignored}
}

fn open_output(int index) any {
   "Opens WinMM output `index`. Returns a stream dict or 0."
   def hp = zalloc(8)
   def rc = int(midiOutOpen(hp, index, 0, 0, _CALLBACK_NULL))
   if rc != _MMSYSERR_NOERROR {
      free(hp)
      return 0
   }
   def h = load64(hp, 0)
   free(hp)
   {"kind": "out", "handle": h, "open": true, "index": index}
}

fn read_bytes(any stream, int timeout_ms=0) list {
   "Drains posted input messages and returns [bytes, ...] chunks in arrival
   order(short messages expanded to their status/data bytes)."
   if !is_dict(stream) || !bool(stream.get("open", false)) { return [] }
   def msg = stream.get("msg", 0)
   def h = stream.get("handle", 0)
   if timeout_ms != 0 {
      def ms = timeout_ms < 0 ? 0xFFFFFFFF : timeout_ms
      def waited = MsgWaitForMultipleObjects(0, 0, 0, ms, _QS_POSTMESSAGE)
      if int(waited) < 0 { return [] }
   }
   mut out = []
   while int(PeekMessageA(msg, 0, _MM_MIM_FIRST, _MM_MIM_LAST, _PM_REMOVE)) != 0 {
      def kind = load32(msg, _MSG_MESSAGE)
      if load64(msg, _MSG_WPARAM) != h { continue }
      def lp = load64(msg, _MSG_LPARAM)
      if kind == _MM_MIM_DATA {
         def status = lp & 255
         def d1 = (lp >> 8) & 127
         def d2 = (lp >> 16) & 127
         def hi = status & 0xF0
         mut n = 3
         if hi == 0xC0 || hi == 0xD0 || status == 0xF1 || status == 0xF3 { n = 2 }
         if status >= 0xF4 { n = 1 }
         if n == 1 { out = out.append([status]) }
         elif n == 2 { out = out.append([status, d1]) }
         else { out = out.append([status, d1, d2]) }
      } elif kind == _MM_MIM_LONGDATA {
         def hdr = lp
         def data = load64(hdr, _HDR_DATA)
         def got = load32(hdr, _HDR_RECORDED)
         mut bytes = []
         mut i = 0
         while i < got {
            bytes = bytes.append(load8(data, i))
            i += 1
         }
         if bytes.len > 0 { out = out.append(bytes) }
         ;; Hand the buffer back unless the stream is closing (reset returns
         ;; every buffer with zero bytes recorded).
         if bool(stream.get("open", false)) { _queue_sysex_buffer(h, hdr) }
      }
   }
   out
}

fn write_bytes(any stream, list bytes) bool {
   "Sends one complete message: short messages through midiOutShortMsg,
   sysex through a prepared long-message buffer."
   if !is_dict(stream) || !bool(stream.get("open", false)) || bytes.len == 0 { return false }
   def h = stream.get("handle", 0)
   def status = int(bytes.get(0, 0)) & 255
   if status != 0xF0 {
      mut packed = status
      if bytes.len > 1 { packed = packed | ((int(bytes.get(1, 0)) & 127) << 8) }
      if bytes.len > 2 { packed = packed | ((int(bytes.get(2, 0)) & 127) << 16) }
      return int(midiOutShortMsg(h, packed)) == _MMSYSERR_NOERROR
   }
   def data = zalloc(bytes.len + 8)
   mut i = 0
   while i < bytes.len {
      store8(data, int(bytes.get(i, 0)) & 255, i)
      i += 1
   }
   def hdr = zalloc(_HDR_SIZE)
   store64(hdr, data, _HDR_DATA)
   store32(hdr, bytes.len, _HDR_LEN)
   store32(hdr, bytes.len, _HDR_RECORDED)
   mut ok = int(midiOutPrepareHeader(h, hdr, _HDR_SIZE)) == _MMSYSERR_NOERROR
   if ok { ok = int(midiOutLongMsg(h, hdr, _HDR_SIZE)) == _MMSYSERR_NOERROR }
   if ok {
      ;; The driver owns the buffer until MHDR_DONE; sysex is rare and
      ;; short, so wait (bounded) instead of tracking async completion.
      mut spins = 0
      while (load32(hdr, _HDR_FLAGS) & _MHDR_DONE) == 0 && spins < 2000 {
         Sleep(1)
         spins += 1
      }
      midiOutUnprepareHeader(h, hdr, _HDR_SIZE)
   }
   free(hdr)
   free(data)
   ok
}

fn close(any stream) bool {
   "Stops and closes a WinMM stream, releasing sysex buffers."
   if !is_dict(stream) || !bool(stream.get("open", false)) { return false }
   stream["open"] = false
   def h = stream.get("handle", 0)
   if to_str(stream.get("kind", "")) == "in" {
      midiInStop(h)
      midiInReset(h)
      def hdrs = stream.get("hdrs", [])
      mut i = 0
      while i < hdrs.len {
         def hdr = hdrs[i]
         midiInUnprepareHeader(h, hdr, _HDR_SIZE)
         free(load64(hdr, _HDR_DATA))
         free(hdr)
         i += 1
      }
      midiInClose(h)
      free(stream.get("msg", 0))
   } else {
      midiOutReset(h)
      midiOutClose(h)
   }
   true
}
