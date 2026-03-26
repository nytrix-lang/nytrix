# Examples

Fenced `ny` blocks are Nytrix source. Complete programs can be copied into a
file and run with `ny`; assertions pass silently.

Each example shows one idea and keeps imports visible. If an example needs a
local service, process, or package file, that requirement is stated before the
code block.

## Script

```ny
use std.core

fn greet(str: name): str {
   "hello, " + name
}

assert_eq(greet("ny"), "hello, ny", "greet")
```

Run:

```bash
ny --color=never script.ny
```

## Collections

```ny
use std.core
use std.core.iter as it

def xs = [1, 2, 3, 4]
assert_eq(it.filter(xs, fn(v){ (v % 2) == 0 }), [2, 4], "even values")
assert_eq(it.reduce(xs, 0, fn(acc, v){ acc + v }), 10, "sum")
```

## JSON

```ny
use std.core
use std.parse.data.json as json

def cfg = json.json_decode("{\"name\":\"ny\",\"debug\":true}")
assert_eq(cfg.get("name", ""), "ny", "name")

def out = json.json_encode({"ok": true})
assert(str_contains(out, "\"ok\""), "json output")
```

## SQL inspection

```ny
use std.core
use std.parse.data.sql as sql

def ast = sql.parse("SELECT id, name FROM users WHERE id = :id")
assert(is_dict(ast), "sql ast")
assert_eq(ast.get("kind", ""), "select", "select kind")
assert_eq(sql.statement_kind("INSERT INTO log(message) VALUES('ok')"), "insert", "insert kind")
```

## Process tube

This example uses `/bin/cat` as a deterministic local echo process.

```ny
use std.core
use std.os.net as net

net.context({"log_level": "quiet", "timeout_ms": 1000, "color": false})

def io = net.process("/bin/cat", [])
net.sendline(io, "ny")
def out = net.recvuntil(io, "ny\n")
net.close(io)

assert_eq(out, "ny\n", "tube echo")
```

## HTTP request

Run this against a local service that accepts `POST /api`.

```ny
use std.core
use std.os.net as net

def r = net.request({
   "method": "POST",
   "url": "http://127.0.0.1:8080/api",
   "json": {"name": "ny"},
   "headers": {"Accept": "application/json"},
   "timeout": 10,
   "follow": true
})

assert(r.get("ok", false), "request ok")
```

## Local server

```text
use std.core
use std.os.net.server as web

fn app(dict: req): dict {
   def path = req.get("path", "/")
   if(path == "/health"){ return web.json({"ok": true}) }
   if(path == "/"){ return web.html("<h1>Nytrix</h1>") }
   web.not_found("not found\n")
}

web.serve_cli(app, {"name": "Ny Server", "port": 8080})
```

Run:

```bash
ny scratch/server.ny
```

## Package manifest

Package manifests are JSON. Put this in `ny.pkg.json` at the project root.

```json
{
  "schema": "ny.pkg.v1",
  "name": "tool",
  "version": "0.1",
  "dependencies": {
    "helper": {"source": "./deps/helper"}
  }
}
```

## Next pages

- [networking.md](networking.md) expands the HTTP, server, socket, and tube APIs.
- [ui.md](ui.md) starts from a window, frame loop, drawing, text, textures, and input.
- [packages.md](packages.md) expands the manifest and resolver model.
- [testing.md](testing.md) covers executable checks and example fences.
