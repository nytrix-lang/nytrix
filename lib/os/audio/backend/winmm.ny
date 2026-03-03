;; Keywords: audio winmm windows io

module std.os.audio.backend.winmm (
   is_available, init, shutdown,
   stream_open, stream_start, stream_stop,
   write
)

use std.core *
use std.core.dict_mod *
use std.os *
use std.os.time *
use std.os.audio.backend.shared as backend_shared
use std.util.common as common

if(comptime{ __os_name() == "windows" }){
   #link "winmm"
   extern fn waveOutOpen(phwo: ptr, uDeviceID: i32, pwfx: ptr, dwCallback: i64, dwInstance: i64, fdwOpen: i32): i32 as "waveOutOpen"
   extern fn waveOutClose(hwo: ptr): i32 as "waveOutClose"
   extern fn waveOutPrepareHeader(hwo: ptr, pwh: ptr, cbwh: i32): i32 as "waveOutPrepareHeader"
   extern fn waveOutUnprepareHeader(hwo: ptr, pwh: ptr, cbwh: i32): i32 as "waveOutUnprepareHeader"
   extern fn waveOutWrite(hwo: ptr, pwh: ptr, cbwh: i32): i32 as "waveOutWrite"
   extern fn waveOutReset(hwo: ptr): i32 as "waveOutReset"
} else {
   fn waveOutOpen(_phwo, _uDeviceID, _pwfx, _dwCallback, _dwInstance, _fdwOpen){
      "Stubbed non-Windows implementation of `waveOutOpen`."
      0
   }
   fn waveOutClose(_hwo){
      "Stubbed non-Windows implementation of `waveOutClose`."
      0
   }
   fn waveOutPrepareHeader(_hwo, _pwh, _cbwh){
      "Stubbed non-Windows implementation of `waveOutPrepareHeader`."
      0
   }
   fn waveOutUnprepareHeader(_hwo, _pwh, _cbwh){
      "Stubbed non-Windows implementation of `waveOutUnprepareHeader`."
      0
   }
   fn waveOutWrite(_hwo, _pwh, _cbwh){
      "Stubbed non-Windows implementation of `waveOutWrite`."
      0
   }
   fn waveOutReset(_hwo){
      "Stubbed non-Windows implementation of `waveOutReset`."
      0
   }
}

def WAVE_FORMAT_PCM = 1

def MMSYSERR_NOERROR = 0
def CALLBACK_NULL = 0

fn is_available(){
   "Returns whether the WinMM backend is available on this host."
   eq(os(), "windows")
}

fn init(ctx){
   "Registers the WinMM backend in the shared audio context."
   backend_shared.init_output_device(ctx, is_available(), "Windows Multimedia", "winmm")
}

fn shutdown(ctx){
   "Shuts down WinMM backend state stored in `ctx`."
   common.touch(ctx)
}

fn stream_open(stream){
   "Opens a WinMM playback stream for the provided stream dictionary."
   def hWaveOut = malloc(8)
   def wfx = malloc(18)
   def format = core.get(stream, "format")
   def sample_rate = core.get(stream, "sample_rate")
   def channels = core.get(stream, "channels")
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
   stream = dict_set(stream, "handle", handle)
   stream = dict_set(stream, "bits_per_sample", bits_per_sample)
   stream
}

fn stream_start(stream){
   "WinMM streams begin playback immediately after the first write."
   common.touch(stream)
   true
}

fn stream_stop(stream){
   "Stops and closes a WinMM playback stream."
   def handle = get(stream, "handle")
   if(handle){
      waveOutReset(handle)
      waveOutClose(handle)
   }
   stream = dict_set(stream, "handle", 0)
   true
}

fn write(handle, buf, bytes){
   "Writes `bytes` of PCM data to a WinMM output stream."
   if(!handle){ return false }
   def hdr_size = 32
   def hdr = malloc(hdr_size)
   memset(hdr, 0, hdr_size)
   store_ptr(hdr, 0, buf)
   store32(hdr, bytes, 8)
   if(waveOutPrepareHeader(handle, hdr, hdr_size) != MMSYSERR_NOERROR){
      free(hdr)
      return false
   }
   if(waveOutWrite(handle, hdr, hdr_size) != MMSYSERR_NOERROR){
      waveOutUnprepareHeader(handle, hdr, hdr_size)
      free(hdr)
      return false
   }
   while((load32(hdr, 24) & 1) == 0){
      msleep(1)
   }
   waveOutUnprepareHeader(handle, hdr, hdr_size)
   free(hdr)
   true
}
