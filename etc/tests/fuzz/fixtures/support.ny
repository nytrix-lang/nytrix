module tmp.test.compiler.language.support(
   dispatch, float_typed, float_inferred, alphabet, alphabet_pick, tuple_pair,
   comp_size, table_size, table_size_explicit, table_comp, jit_kind
)

def MODE_TRIANGLES = 4
def MODE_STRIP = 5
def MODE_FAN = 6
def BYTE = 5120
def UBYTE = 5121
def SHORT = 5122
def USHORT = 5123
def UINT = 5125
def FLOAT = 5126
def UPPER = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
def LOWER = "abcdefghijklmnopqrstuvwxyz"
def DIGITS = "0123456789"

comptime table CompSize {
   BYTE, UBYTE -> 1
   SHORT, USHORT -> 2
   UINT, FLOAT -> 4
   _ -> 0
}

fn dispatch(int mode) int {
   case mode {
      MODE_TRIANGLES -> 3
      MODE_STRIP -> 6
      MODE_FAN -> 9
      _ -> -1
   }
}

fn float_typed(number px) float { 2048.0 / px }

fn float_inferred(number px){ 2048.0 / px }

fn alphabet() str { UPPER + LOWER + DIGITS + "+/" }

fn alphabet_pick(int i) int { load8(alphabet(), i) }

fn tuple_pair() list {
   def a, b = 11, 22
   [a, b]
}

@jit
fn comp_size(int comp) int { _comp_size(comp) }

fn _resolve_table(any acc, bool explicit=false) dict {
   def comp = acc.get("componentType", 0)
   def size = explicit ? _comp_size(comp, 0) : _comp_size(comp)
   {"comp": comp, "size": size}
}

fn table_size() int { int(_resolve_table({"componentType": USHORT}).get("size", -1)) }

fn table_size_explicit() int { int(_resolve_table({"componentType": USHORT}, true).get("size", -1)) }

fn table_comp() int { int(_resolve_table({"componentType": USHORT}).get("comp", -1)) }

@jit
fn jit_kind(int kind) int {
   case kind {
      USHORT -> 16
      UINT -> 32
      UBYTE -> 8
      _ -> 0
   }
}
