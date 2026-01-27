use std.io
use std.core.error

;; std.io.mod (Test)
;; Tests basic IO printing and file operations.

print("Testing io.mod...")

assert(print("IO Test") == 0, "print return 0")

def tmp_file = "/tmp/nytrix_io_test.txt"
file_write(tmp_file, "hello io")
assert(file_exists(tmp_file), "exists after write")
assert(eq(file_read(tmp_file), "hello io"), "read matches")

file_append(tmp_file, " append")
assert(eq(file_read(tmp_file), "hello io append"), "append matches")

file_remove(tmp_file)
assert(!file_exists(tmp_file), "removed")

print("✓ std.io.mod tests passed")
