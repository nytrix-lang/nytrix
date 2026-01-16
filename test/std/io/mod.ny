use std.io
use std.core
use std.core.test

print("Testing io.mod...")

; Test print
assert(print("IO Test") == 0, "print return 0")

; Test write_fd/read_fd basics
def tmp_file = "/tmp/nytrix_io_test.txt"
file_write(tmp_file, "hello io")
assert(file_exists(tmp_file), "file_exists after write")
assert(eq(file_read(tmp_file), "hello io"), "file_read matches")

file_append(tmp_file, " append")
assert(eq(file_read(tmp_file), "hello io append"), "file_append matches")

file_remove(tmp_file)
assert(!file_exists(tmp_file), "file_remove works")

print("✓ std.io.mod tests passed")
