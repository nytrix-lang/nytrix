use std.core *
use std.core.list *
use std.os *
use std.str *

;; Core (Test)
;; Tests core memory, I/O operations, list operations, and type checking.

; Memory operations
def ptr = malloc(64)
assert(ptr != 0, "malloc returns non-null")
store64(ptr, 12345)
mut val = load64(ptr)
assert(val == 12345, "store64/load64")
store8(ptr, 255)
val = load8(ptr)
assert(val == 255, "store8/load8")
free(ptr)

; List operations
mut lst = list(8)
assert(is_list(lst), "list creation")
assert(len(lst) == 0, "empty list length")
lst = append(lst, 10)
lst = append(lst, 20)
lst = append(lst, 30)
assert(len(lst) == 3, "list length after appends")
assert(get(lst, 0) == 10, "get first element")
assert(get(lst, 1) == 20, "get second element")
assert(get(lst, 2) == 30, "get third element")
assert(get(lst, -1) == 30, "negative indexing")
set_idx(lst, 1, 25)
assert(get(lst, 1) == 25, "set element")
val = pop(lst)
assert(val == 30, "pop returns last element")
assert(len(lst) == 2, "length after pop")
def lst2 = [40, 50]
lst = extend(lst, lst2)
assert(len(lst) == 4, "extend length")
lst = list_clear(lst)
assert(len(lst) == 0, "clear list")

; Type checking
assert(is_int(42), "is_int on integer")
assert(!is_int("string"), "is_int on string")
assert(is_ptr("string"), "is_ptr on string")
assert(!is_ptr(42), "is_ptr on integer")
assert(is_list([1, 2, 3]), "is_list on list")
assert(!is_list(42), "is_list on integer")
def d = dict(8)
assert(is_dict(d), "is_dict on dict")
assert(!is_dict([]), "is_dict on list")

; 'in' operator
def in_list = [1, 2, 3]
assert(contains(in_list, 1), "in operator on list")
assert(!contains(in_list, 4), "in operator on list (not found)")
def in_str = "hello"
assert(contains(in_str, "ell"), "in operator on string")
assert(!contains(in_str, "xyz"), "in operator on string (not found)")

; File operations
def test_file = "/tmp/nytrix_core_test.txt"
def test_data = "Hello, Nytrix!"
def result = file_write(test_file, test_data)
assert(is_ok(result), "file_write returns ok")
assert(__unwrap(result) > 0, "file_write returns bytes written")
assert(file_exists(test_file), "file exists after write")
mut r = file_read(test_file)
assert(is_ok(r), "file_read returns ok")
mut content = __unwrap(r)
assert(eq(content, test_data), "file content matches")
unwrap(file_append(test_file, " More data"))
r = file_read(test_file)
assert(is_ok(r), "file_read(2) returns ok")
content = __unwrap(r)
assert(str_contains(content, "More data"), "file append works")
unwrap(file_remove(test_file))
assert(!file_exists(test_file), "file removed")

; String helpers
def s1 = "hello"
def s2 = "hello"
def s3 = "world"
assert(_str_eq(s1, s2), "string equality")
assert(!_str_eq(s1, s3), "string inequality")

print("✓ std.core.mod tests passed")
