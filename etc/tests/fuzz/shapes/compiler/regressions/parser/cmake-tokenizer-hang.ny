;; NY-007: std.math.parse.syntax CMake tokenization hangs on a tiny CMakeLists.txt
;; shaped input.
;;
;; Repro:
;;   ny --compiler-asserts -c "$(cat etc/tests/fuzz/errors/parser/cmake-tokenizer-hang.ny)"
;;
;; Current local bad signal on 2026-06-01:
;;   timeout after 5s while evaluating tokenize_auto.

use std.core
use std.math.parse.syntax as syn

def s = "cmake_minimum_required(VERSION 3.10)\nproject(x)\n"
def toks = syn.tokenize_auto(s, "CMakeLists.txt", list(0))
print(len(toks))
