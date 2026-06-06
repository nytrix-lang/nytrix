;; Keywords: sound backend winmm os
;; WinMM sound backend for Windows audio playback.
;; References:
;; - std.os.sound.backend
;; - std.os
module std.os.sound.backend.winmm(is_available, init, shutdown, stream_open, stream_start, stream_stop, write)
use std.core
use std.core.dict_mod
use std.os
use std.os.time
use std.os.sound.backend.shared as backend_shared

#windows {
   #link "winmm"
   #include <windows.h>
   #include <mmsystem.h>
   extern "" {
      fn waveOutOpen(ptr _phwo, u32 _uDeviceID, ptr _pwfx, ptr _dwCallback, ptr _dwInstance, u32 _fdwOpen) u32
      fn waveOutClose(ptr _hwo) u32
      fn waveOutPrepareHeader(ptr _hwo, ptr _pwh, u32 _cbwh) u32
      fn waveOutUnprepareHeader(ptr _hwo, ptr _pwh, u32 _cbwh) u32
      fn waveOutWrite(ptr _hwo, ptr _pwh, u32 _cbwh) u32
      fn waveOutReset(ptr _hwo) u32
   }
} #else {
   "Runs the waveOutReset operation."
   fn waveOutOpen(any _phwo, any _uDeviceID, any _pwfx, any _dwCallback, any _dwInstance, any _fdwOpen) int {
      "Stubbed non-Windows implementation of `waveOutOpen`."
      0
   }
   fn waveOutClose(any _hwo) int {
      "Stubbed non-Windows implementation of `waveOutClose`."
      0
   }
   fn waveOutPrepareHeader(any _hwo, any _pwh, any _cbwh) int {
      "Stubbed non-Windows implementation of `waveOutPrepareHeader`."
      0
   }
   fn waveOutUnprepareHeader(any _hwo, any _pwh, any _cbwh) int {
      "Stubbed non-Windows implementation of `waveOutUnprepareHeader`."
      0
   }
   fn waveOutWrite(any _hwo, any _pwh, any _cbwh) int {
      "Stubbed non-Windows implementation of `waveOutWrite`."
      0
   }
   fn waveOutReset(any _hwo) int {
      "Stubbed non-Windows implementation of `waveOutReset`."
      0
   }
} #endif
def WAVE_FORMAT_PCM = 1
def MMSYSERR_NOERROR = 0
def CALLBACK_NULL = 0

fn is_available() bool {
   "Returns whether the WinMM backend is available on this host."
   #windows {
      true
   } #else {
      false
   } #endif
}

fn init(any ctx) any {
   "Registers the WinMM backend in the shared audio context."
   backend_shared.init_output_device(ctx, is_available(), "Windows Multimedia", "winmm")
}

fn shutdown(any ctx) any { "Shuts down WinMM backend state stored in `ctx`." }

fn stream_open(any stream) any {
   "Opens a WinMM playback stream for the provided stream dictionary."
   def hWaveOut = malloc(8)
   def wfx = malloc(18)
   def format = stream.get("format")
   def sample_rate = stream.get("sample_rate")
   def channels = stream.get("channels")
   def bits_per_sample = (format == 2) ? 32 : 16
   store16(wfx, WAVE_FORMAT_PCM, 0)
   store16(wfx, channels, 2)
   store32(wfx, sample_rate, 4)
   store32(wfx, sample_rate * channels * (bits_per_sample / 8), 8)
   store16(wfx, channels * (bits_per_sample / 8), 12)
   store16(wfx, bits_per_sample, 14)
   store16(wfx, 0, 16)
   def res = waveOutOpen(hWaveOut, -1, wfx, 0, 0, CALLBACK_NULL)
   free(wfx)
   if(res != MMSYSERR_NOERROR){
      free(hWaveOut)
      return false
   }
   def handle = load64(hWaveOut, 0)
   free(hWaveOut)
   stream = stream.set("handle", handle)
   stream = stream.set("bits_per_sample", bits_per_sample)
   stream
}

fn stream_start(any stream) bool {
   "WinMM streams begin playback immediately after the first write."
   true
}

fn stream_stop(any stream) bool {
   "Stops and closes a WinMM playback stream."
   def handle = stream.get("handle")
   if(handle){
      waveOutReset(handle)
      waveOutClose(handle)
   }
   stream = stream.set("handle", 0)
   true
}

fn write(any h, any buf, int bytes) bool {
   "Writes `bytes` of PCM data to a WinMM output stream."
   if(!h){ return false }
   def hdr_size = 32
   def hdr = malloc(hdr_size)
   memset(hdr, 0, hdr_size)
   store64(hdr, buf, 0)
   store32(hdr, bytes, 8)
   if(waveOutPrepareHeader(h, hdr, hdr_size) != MMSYSERR_NOERROR){
      free(hdr)
      return false
   }
   if(waveOutWrite(h, hdr, hdr_size) != MMSYSERR_NOERROR){
      waveOutUnprepareHeader(h, hdr, hdr_size)
      free(hdr)
      return false
   }
   while((load32(hdr, 24) & 1) == 0){ msleep(1) }
   waveOutUnprepareHeader(h, hdr, hdr_size)
   free(hdr)
   true
}
