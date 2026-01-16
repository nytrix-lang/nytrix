;;; convert.ny --- util convert module

;; Keywords: util convert

;;; Commentary:

;; Util Convert module.

module std.util.convert (
	int_to_str, parse_int, to_bool
)

fn int_to_str(n) {
	"Convert integer/float to string."
	if (n == 0) { return "0" }
	def negative = 0
	if (n < 0) {
		negative = 1
		n = -n
	}
	def temp = n
	def digits = 0
	while (temp > 0) {
		digits = digits + 1
		temp = temp / 10
	}
	def size = digits
	size = digits
	if (negative) { size = size + 1  }
	def buf = rt_malloc(size + 1)
	rt_init_str(buf, size) ; Tag String + Len
	def pos = size
	rt_store8_idx(buf, pos, 0)
	pos = pos - 1
	while (n > 0) {
		def digit = n % 10
		rt_store8_idx(buf, pos, 48 + digit)
		pos = pos - 1
		n = n / 10
	}
	if (negative) {
		rt_store8_idx(buf, 0, 45)
	}
	return buf
}

fn parse_int(s) {
	"Convert string to int."
	if (type(s) == "int") { return s  }
	def n = len(s)
	if (n == 0) { return 0  }
	def result = 0
	def negative = 0
	def start = 0
	if (rt_load8_idx(s, 0) == 45) {
		negative = 1
		start = 1
	}
	def i = start
	while (i < n) {
		def c = rt_load8_idx(s, i)
		if (c >= 48 && c <= 57) {
			result = result * 10 + (c - 48)
		}
		i = i + 1
	}
	if (negative) { return -result  }
	return result
}

fn to_bool(val) {
	"Convert to bool."
	def t = type(val)
	if (t == "bool") { return val  }
	if (t == "int") { return val != 0  }
	if (t == "str") { return len(val) > 0  }
	if (t == "list") { return list_len(val) > 0  }
	return val != 0
}
