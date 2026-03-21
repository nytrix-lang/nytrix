;; repl-expect: REPL_LONG_DOC_SIGNATURE_OK
use std.core

fn repl_long_doc_signature(
   int: p00, int: p01, int: p02, int: p03, int: p04, int: p05,
   int: p06, int: p07, int: p08, int: p09, int: p10, int: p11,
   int: p12, int: p13, int: p14, int: p15, int: p16, int: p17,
   int: p18, int: p19, int: p20, int: p21, int: p22, int: p23,
   int: p24, int: p25, int: p26, int: p27, int: p28, int: p29,
   int: p30, int: p31, int: p32, int: p33, int: p34, int: p35,
   int: p36, int: p37, int: p38, int: p39, int: p40, int: p41,
   int: p42, int: p43, int: p44, int: p45, int: p46, int: p47,
   int: p48, int: p49, int: p50, int: p51, int: p52, int: p53,
   int: p54, int: p55, int: p56, int: p57, int: p58, int: p59
): int {
   "Forces REPL doc collection to retain a signature longer than the old fixed stack buffer."
   1
}

fn main(): any {
   assert(repl_long_doc_signature(
         0, 1, 2, 3, 4, 5,
         6, 7, 8, 9, 10, 11,
         12, 13, 14, 15, 16, 17,
         18, 19, 20, 21, 22, 23,
         24, 25, 26, 27, 28, 29,
         30, 31, 32, 33, 34, 35,
         36, 37, 38, 39, 40, 41,
         42, 43, 44, 45, 46, 47,
         48, 49, 50, 51, 52, 53,
         54, 55, 56, 57, 58, 59
   ) == 1, "long REPL doc signature survives paste")
   print("REPL_LONG_DOC_SIGNATURE_OK")
}
