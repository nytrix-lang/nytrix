;; Keywords: cipher morse math crypto
;; Classical cipher routines for Morse code encoding and decoding.
;; Supports standard ITU-R Morse code.
;; Reference:
;; - https://www.itu.int/rec/R-REC-M.1677/en
;; References:
;; - std.math.crypto.cipher
;; - std.math.crypto.analysis
module std.math.crypto.cipher.morse(morse_encode, morse_decode, morse_encode_char, morse_decode_symbol)
use std.core
use std.core.str

def _MORSE_PAIRS = ["A", ".-", "B", "-...", "C", "-.-.", "D", "-..", "E", ".", "F", "..-.", "G", "--.", "H", "....", "I", "..", "J", ".---", "K", "-.-", "L", ".-..", "M", "--", "N", "-.", "O", "---", "P", ".--.", "Q", "--.-", "R", ".-.", "S", "...", "T", "-", "U", "..-", "V", "...-", "W", ".--", "X", "-..-", "Y", "-.--", "Z", "--..", "0", "-----", "1", ".----", "2", "..---", "3", "...--", "4", "....-", "5", ".....", "6", "-....", "7", "--...", "8", "---..", "9", "----.", ".", ".-.-.-", ",", "--..--", "?", "..--..", "'", ".----.", "!", "-.-.--", "/", "-..-.", "(", "-.--.", ")", "-.--.-", "&", ".-...", ":", "---...", ";", "-.-.-.", "=", "-...-", "+", ".-.-.", "-", "-....-", "_", "..--.-", "\"", ".-..-.", "$", "...-..-", "@", ".--.-.",]

fn _morse_table() dict {
   mut t, i = dict(64), 0
   while(i < _MORSE_PAIRS.len){
      t.set(_MORSE_PAIRS[i], _MORSE_PAIRS[i + 1])
      i += 2
   }
   t
}

fn _morse_reverse_table() dict {
   mut r, i = dict(64), 0
   while(i < _MORSE_PAIRS.len){
      r.set(_MORSE_PAIRS[i + 1], _MORSE_PAIRS[i])
      i += 2
   }
   r
}

fn morse_encode_char(str ch) str {
   "Encode a single character to its Morse code symbol.
   Returns the Morse string(e.g. '.-') or '?' if unknown."
   def t = _morse_table()
   def uc = upper(ch)
   t.get(uc, "?")
}

fn morse_encode(str text) str {
   "Encode a text string to Morse code.
   Letters are separated by spaces, words by ' / '.
   Returns the Morse code string."
   def t = _morse_table()
   mut out = ""
   mut i = 0
   mut word_sep = false
   while(i < text.len){
      def ch = utf8_slice(text, i, i + 1, 1)
      if(ch == " "){
         if(!word_sep){
            out = str_add(out, " / ")
            word_sep = true
         }
      } else {
         def uc = upper(ch)
         def sym = t.get(uc, "")
         if(sym.len > 0){
            if(out.len > 0 && !word_sep){ out = str_add(out, " ") }
            out = str_add(out, sym)
            word_sep = false
         }
      }
      i += 1
   }
   out
}

fn morse_decode(str morse) str {
   "Decode a Morse code string to plain text.
   Symbols separated by spaces, words by ' / ' or '  '.
   Returns the decoded text(uppercase)."
   def r = _morse_reverse_table()
   def words = split(morse, " / ")
   mut out = ""
   mut wi = 0
   while(wi < words.len){
      def word = words[wi]
      if(word.len > 0){
         if(wi > 0){ out = str_add(out, " ") }
         def syms = split(word, " ")
         mut si = 0
         while(si < syms.len){
            def sym = syms[si]
            if(sym.len > 0){
               def ch = r.get(sym, "?")
               out = str_add(out, ch)
            }
            si += 1
         }
      }
      wi += 1
   }
   out
}

fn morse_decode_symbol(str sym) str {
   "Decode a single Morse symbol like '.-' to its character.
   Returns the character or '?' if unknown."
   def r = _morse_reverse_table()
   r.get(sym, "?")
}
