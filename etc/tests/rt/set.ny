use std.core

mut strings = set()
mut i = 0
while(i < 300){
   strings = strings.add("key-" + to_str(i))
   i += 1
}

assert(strings.contains("key-42"), "set contains dynamic string before duplicate")
strings = strings.add("key-42")
assert(strings.len == 300, "set duplicate string after many inserts must not grow")
mut ips = set()
ips = ips.add("227.15.198.129")
ips = ips.add("227.15.198.129")
assert(ips.len == 1, "set duplicate literal string must not grow")
print("✓ set tests passed")
