use std.core

assert(__nanosleep(0) == -1, "__nanosleep rejects tagged zero timespec")
assert(__write_buffered(-1, "x", 1) < 0, "__write_buffered reports invalid fd")
assert(__print_flush() == 1, "__print_flush returns tagged success")

print("✓ runtime io tests passed")
