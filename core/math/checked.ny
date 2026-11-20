;; Keywords: math integer checked overflow
;; Explicit checked arithmetic for signed 64-bit Nytrix integers.
;; References:
;; - core.math
module core.math.checked(checked_add, checked_sub, checked_mul, checked_div,
   checked_neg, saturating_add, saturating_sub, saturating_mul)
use core
use core.error (ERR_OVERFLOW, ERR_DIV_ZERO, ERR_TYPE, exception)
use core.math.big (bigint, bigint_add, bigint_sub, bigint_mul, bigint_div,
   bigint_neg, bigint_to_int, bigint_lt, bigint_gt)

def _I64_MIN = bigint("-9223372036854775808")
def _I64_MAX = bigint("9223372036854775807")

fn _failure(str kind, str message) any { err(exception(kind, message)) }

fn _int_pair(any a, any b) bool { is_int(a) && is_int(b) }

fn _checked(any value, str operation) any {
   def v = bigint(value)
   if bigint_lt(v, _I64_MIN) || bigint_gt(v, _I64_MAX) {
      return _failure(ERR_OVERFLOW, "checked " + operation + " overflow")
   }
   ok(bigint_to_int(v))
}

fn checked_add(any a, any b) any {
   "Return Ok(a + b), or Err(err.overflow); operands must be machine ints."
   if !_int_pair(a, b) { return _failure(ERR_TYPE, "checked_add expects int operands") }
   _checked(bigint_add(bigint(a), bigint(b)), "addition")
}

fn checked_sub(any a, any b) any {
   "Return Ok(a - b), or Err(err.overflow); operands must be machine ints."
   if !_int_pair(a, b) { return _failure(ERR_TYPE, "checked_sub expects int operands") }
   _checked(bigint_sub(bigint(a), bigint(b)), "subtraction")
}

fn checked_mul(any a, any b) any {
   "Return Ok(a * b), or Err(err.overflow); operands must be machine ints."
   if !_int_pair(a, b) { return _failure(ERR_TYPE, "checked_mul expects int operands") }
   _checked(bigint_mul(bigint(a), bigint(b)), "multiplication")
}

fn checked_div(any a, any b) any {
   "Return truncating Ok(a / b), or Err for zero divisor or overflow."
   if !_int_pair(a, b) { return _failure(ERR_TYPE, "checked_div expects int operands") }
   def int left, right = int(a), int(b)
   if right == 0 { return _failure(ERR_DIV_ZERO, "checked_div by zero") }
   if bigint_to_int(_I64_MIN) == left && right == -1 {
      return _failure(ERR_OVERFLOW, "checked division overflow")
   }
   ok(left / right)
}

fn checked_neg(any a) any {
   "Return Ok(-a), or Err(err.overflow) for the minimum machine int."
   if !is_int(a) { return _failure(ERR_TYPE, "checked_neg expects an int") }
   _checked(bigint_neg(bigint(a)), "negation")
}

fn _saturate(any value) int {
   def v = bigint(value)
   if bigint_lt(v, _I64_MIN) { return bigint_to_int(_I64_MIN) }
   if bigint_gt(v, _I64_MAX) { return bigint_to_int(_I64_MAX) }
   bigint_to_int(v)
}

fn saturating_add(any a, any b) any {
   "Add machine ints, clamping overflow to the nearest signed 64-bit bound."
   if !_int_pair(a, b) { return _failure(ERR_TYPE, "saturating_add expects int operands") }
   _saturate(bigint_add(bigint(a), bigint(b)))
}

fn saturating_sub(any a, any b) any {
   "Subtract machine ints, clamping overflow to the nearest signed 64-bit bound."
   if !_int_pair(a, b) { return _failure(ERR_TYPE, "saturating_sub expects int operands") }
   _saturate(bigint_sub(bigint(a), bigint(b)))
}

fn saturating_mul(any a, any b) any {
   "Multiply machine ints, clamping overflow to the nearest signed 64-bit bound."
   if !_int_pair(a, b) { return _failure(ERR_TYPE, "saturating_mul expects int operands") }
   _saturate(bigint_mul(bigint(a), bigint(b)))
}
