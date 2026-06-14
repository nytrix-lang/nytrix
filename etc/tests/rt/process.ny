use std.core

def argv_before = __argvp()
assert(argv_before != 0, "__argvp returns the startup argv snapshot")
assert(__fix_fn_ptr(nil) == nil, "__fix_fn_ptr accepts nil")
assert(__fix_fn_ptr(0) == 0, "__fix_fn_ptr preserves tagged zero")
def probe_enabled = __index_read_probe_enabled()
assert(probe_enabled == true || probe_enabled == false, "__index_read_probe_enabled returns a bool")
assert(__set_args(0, 0, 0) == nil, "__set_args decodes source integers")
assert(__argc() == 0, "__set_args installs an empty argv")
assert(__argvp() != 0, "__argvp returns the empty argv snapshot")
assert(__argv(0) == nil, "__argv returns nil outside the empty argv")
print("✓ runtime process tests passed")
