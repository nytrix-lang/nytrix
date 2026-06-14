;; Keywords: encoding scream math crypto
;; Encoding routines for Unicode steganography encoding and decoding.
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc7468
;; References:
;; - std.math.crypto.encoding
;; - std.math.crypto
module std.math.crypto.encoding.scream(scream_mark_decode, scream_mark_from_codepoint, scream_extract_marks, scream_decode_marks, scream_decode_text)
use std.core
use std.core.str

fn _scream_mark_index(int mark) int {
   if mark == 0 { return 0 }
   if mark == 0x0307 { return 1 }
   if mark == 0x0327 { return 2 }
   if mark == 0x0331 || mark == 0x0332 { return 3 }
   if mark == 0x0301 { return 4 }
   if mark == 0x032e { return 5 }
   if mark == 0x030b { return 6 }
   if mark == 0x0330 { return 7 }
   if mark == 0x0309 { return 8 }
   if mark == 0x0313 { return 9 }
   if mark == 0x0323 { return 10 }
   if mark == 0x0306 { return 11 }
   if mark == 0x030c { return 12 }
   if mark == 0x0302 { return 13 }
   if mark == 0x030a { return 14 }
   if mark == 0x032f { return 15 }
   if mark == 0x0324 { return 16 }
   if mark == 0x0311 { return 17 }
   if mark == 0x0303 { return 18 }
   if mark == 0x0304 { return 19 }
   if mark == 0x0308 { return 20 }
   if mark == 0x0300 { return 21 }
   if mark == 0x030f { return 22 }
   if mark == 0x033d { return 23 }
   if mark == 0x0326 { return 24 }
   if mark == 0x023a { return 25 }
   -1
}

fn _scream_mark_by_index(int idx) int {
   if idx == 0 { return 0 }
   if idx == 1 { return 0x0307 }
   if idx == 2 { return 0x0327 }
   if idx == 3 { return 0x0331 }
   if idx == 4 { return 0x0301 }
   if idx == 5 { return 0x032e }
   if idx == 6 { return 0x030b }
   if idx == 7 { return 0x0330 }
   if idx == 8 { return 0x0309 }
   if idx == 9 { return 0x0313 }
   if idx == 10 { return 0x0323 }
   if idx == 11 { return 0x0306 }
   if idx == 12 { return 0x030c }
   if idx == 13 { return 0x0302 }
   if idx == 14 { return 0x030a }
   if idx == 15 { return 0x032f }
   if idx == 16 { return 0x0324 }
   if idx == 17 { return 0x0311 }
   if idx == 18 { return 0x0303 }
   if idx == 19 { return 0x0304 }
   if idx == 20 { return 0x0308 }
   if idx == 21 { return 0x0300 }
   if idx == 22 { return 0x030f }
   if idx == 23 { return 0x033d }
   if idx == 24 { return 0x0326 }
   if idx == 25 { return 0x023a }
   -1
}

fn _scream_precomposed_index(int code) int {
   if code == 0x1ec8 { return 8 }
   if code == 0x00c9 { return 4 }
   if code == 0x00ce { return 13 }
   if code == 0x1ef8 { return 18 }
   if code == 0x1e02 { return 1 }
   if code == 0x00dc { return 20 }
   if code == 0x0147 { return 12 }
   if code == 0x00cb { return 20 }
   if code == 0x1ebc { return 18 }
   if code == 0x0228 { return 2 }
   if code == 0x1e54 { return 4 }
   -1
}

fn scream_mark_decode(int mark) str {
   "Decode one Scream Cipher combining-mark codepoint to A-Z. Use 0 for bare A."
   def idx = _scream_mark_index(mark)
   idx >= 0 ? chr(65 + idx) : "?"
}

fn scream_mark_from_codepoint(int code) int {
   "Return a Scream Cipher mark from a combining or known precomposed codepoint.
   Returns -1 when the codepoint is not a supported mark."
   if code != 0 {
      def idx = _scream_mark_index(code)
      if idx >= 0 { return _scream_mark_by_index(idx) }
   }
   def pidx = _scream_precomposed_index(code)
   if pidx >= 0 { return _scream_mark_by_index(pidx) }
   -1
}

fn _scream_is_ascii_letter(int code) bool {
   (code >= 65 && code <= 90) || (code >= 97 && code <= 122)
}

fn _scream_is_combining_mark(int code) bool {
   code >= 0x0300 && code <= 0x033d
}

fn scream_extract_marks(str text) list {
   "Extract one Scream Cipher mark per visible letter from UTF-8 text.
   ASCII letters without a following combining mark become bare-A marks(0)."
   mut marks = []
   mut i = 0
   while i < text.utf8_len {
      def code = text.ord_at(i)
      def mark = scream_mark_from_codepoint(code)
      if mark >= 0 {
         if _scream_is_combining_mark(code) && marks.len > 0 && marks[marks.len - 1] == 0 { marks[marks.len - 1] = mark }
         else { marks = marks.append(mark) }
      } elif _scream_is_ascii_letter(code) {
         marks = marks.append(0)
      }
      i += 1
   }
   marks
}

fn scream_decode_marks(list marks) str {
   "Decode a list of Scream Cipher combining-mark codepoints. Use 0 for bare A."
   mut out = Builder(max(8, marks.len + 1))
   mut i = 0
   while i < marks.len {
      def idx = _scream_mark_index(int(marks[i]))
      out = builder_append_byte(out, idx >= 0 ? 65 + idx : 63)
      i += 1
   }
   def text = builder_to_str(out)
   builder_free(out)
   text
}

fn scream_decode_text(str text) str {
   "Decode Scream Cipher directly from UTF-8 carrier text."
   mut out = Builder(max(8, text.utf8_len + 1))
   mut last_ascii_letter = 0
   mut emitted = false
   mut pending_bare = false
   mut i = 0
   while i < text.utf8_len {
      def code = text.ord_at(i)
      if code == 32 || code == 9 || code == 10 || code == 13 {
         if pending_bare {
            out = builder_append_byte(out, 65)
            pending_bare = false
            emitted = true
         }
         if emitted {
            out = builder_append_byte(out, 95)
            emitted = false
         }
         last_ascii_letter = 0
      } elif _scream_is_ascii_letter(code) {
         if pending_bare {
            out = builder_append_byte(out, 65)
            emitted = true
         }
         last_ascii_letter = code
         pending_bare = true
      } elif _scream_is_combining_mark(code) {
         mut idx = _scream_mark_index(code)
         if last_ascii_letter == 76 && code == 0x0302 { idx = 17 }
         if pending_bare && idx >= 0 {
            out = builder_append_byte(out, 65 + idx)
            pending_bare = false
            emitted = true
         }
         last_ascii_letter = 0
      } else {
         def pidx = _scream_precomposed_index(code)
         if pidx >= 0 {
            if pending_bare { out = builder_append_byte(out, 65) }
            out = builder_append_byte(out, 65 + pidx)
            pending_bare = false
            emitted = true
            last_ascii_letter = 0
         } elif pending_bare {
            out = builder_append_byte(out, 65)
            pending_bare = false
            emitted = true
            last_ascii_letter = 0
         }
      }
      i += 1
   }
   if pending_bare { out = builder_append_byte(out, 65) }
   def decoded = builder_to_str(out)
   builder_free(out)
   decoded
}
