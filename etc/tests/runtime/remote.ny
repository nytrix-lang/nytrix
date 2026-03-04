use std.core *
use std.os *
use std.str.io *

print("Testing remote includes...")
use "https://raw.githubusercontent.com/nytrix-lang/nytrix/main/etc/tests/args.ny" as RemoteArgs
print("✓ Remote include worked!")

print("Testing remote fetch...")
def url = "http://example.com"
def text = fetch(url)
def n = str_len(text)
print(f"Fetched URL: {url}, length: {n}")
if(n >= 10){
   print("Fetched content preview:")
   print(slice(text, 0, 60, 1) + "...")
} else {
   print(f"DEBUG: fetch returned: '{text}'")
   panic(f"Failed to fetch {url} or content too short.")
}

print("✓ All remote tests passed")
