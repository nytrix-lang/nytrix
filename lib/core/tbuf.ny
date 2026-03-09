;; Keywords: tbuf text-buffer
;; Typed buffer primitives for hot numeric loops.
;; Unlike lists, typed buffers store raw untagged values in contiguous memory.
module std.core.tbuf(f32buf_new, f32buf_load, f32buf_store, f32buf_load_raw, f32buf_store_raw, f64buf_new, f64buf_load, f64buf_store, u8buf_new, u8buf_load, u8buf_store, i32buf_new, i32buf_load, i32buf_store, i64buf_new, i64buf_load, i64buf_store, tbuf_len, tbuf_ptr, tbuf_copy)
use std.core

fn _tbuf_new(int: n, int: elem_size): ptr {
   if(n < 0){ n = 0 }
   if(elem_size <= 0){ elem_size = 1 }
   def total = 16 + n * elem_size
   def base = malloc(total)
   if(!base){ panic("typed buffer allocation failed") }
   store64(base, n, 0)
   store64(base, elem_size, 8)
   memset(base + 16, 0, n * elem_size)
   base + 16
}

fn f32buf_new(int: n): ptr { _tbuf_new(n, 4) }

fn f64buf_new(int: n): ptr { _tbuf_new(n, 8) }

fn u8buf_new(int: n): ptr { _tbuf_new(n, 1) }

fn i32buf_new(int: n): ptr { _tbuf_new(n, 4) }

fn i64buf_new(int: n): ptr { _tbuf_new(n, 8) }

fn f32buf_load(any: buf, int: i): f32 { load32_f32(buf, i * 4) }

fn f64buf_load(any: buf, int: i): f64 { load64_f64(buf, i * 8) }

fn u8buf_load(any: buf, int: i): int { load8(buf, i) }

fn i32buf_load(any: buf, int: i): int {
   "Loads a signed 32-bit integer from `buf` at index `i`."
   def v = load32(buf, i * 4)
   v >= 0x80000000 ? v - 0x100000000 : v
}

fn i64buf_load(any: buf, int: i): int { load64(buf, i * 8) }

fn f32buf_store(any: buf, int: i, f32: v): any { store32_f32(buf, v, i * 4) }

fn f64buf_store(any: buf, int: i, f64: v): any { store64_f64(buf, v, i * 8) }

fn u8buf_store(any: buf, int: i, int: v): any { store8(buf, v, i) }

fn i32buf_store(any: buf, int: i, int: v): any { store32(buf, v, i * 4) }

fn i64buf_store(any: buf, int: i, int: v): any { store64(buf, v, i * 8) }

@inline
fn f32buf_load_raw(any: buf, int: i): int {
   "Loads the raw IEEE-754 bit pattern of the `f32` value at index `i`."
   load32(buf, i * 4)
}

@inline
fn f32buf_store_raw(any: buf, int: i, int: bits): any {
   "Stores raw IEEE-754 bits at index `i` without converting through `f32`."
   store32(buf, bits, i * 4)
}

@inline
fn tbuf_len(any: buf): int {
   "Returns the logical element count for typed buffer `buf`."
   if(!buf){ return 0 }
   load64(buf - 16, 0)
}

@inline
fn tbuf_ptr(any: buf): ptr {
   "Returns the raw backing pointer for typed buffer `buf`."
   buf
}

@inline
fn tbuf_copy(any: dst, int: di, any: src, int: si, int: n, int: elem_size): any {
   "Copies `n` elements of `elem_size` bytes from `src[si]` into `dst[di]`."
   if(!dst || !src || n <= 0 || elem_size <= 0){ return dst }
   memcpy(dst + di * elem_size, src + si * elem_size, n * elem_size)
   dst
}
