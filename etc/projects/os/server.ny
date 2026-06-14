#!/usr/bin/env ny

;; Keywords: os http server routes html json example
;; Small HTTP server with HTML, CSS, text, and JSON routes.
use std.core
use std.os (exit)
use std.os.net.server as web

def HTML = "<!doctype html><html lang=en>
<meta charset=utf-8>
<meta name=viewport content='width=device-width,initial-scale=1'>
<title>Ny Server</title>
<link rel=stylesheet href=/style.css>
<main>
<header>
<p>Nytrix HTTP</p>
<h1>It works.</h1>
</header>
<section>
<p>Served by <code>etc/projects/os/server.ny</code></p>
<nav>
<a href=/health>health</a>
<a href=/plain>plain</a>
<a href=/style.css>css</a>
</nav>
</section>
</main>
</html>"
def CSS = ":root{--a:#9b5cff;--e:#3a2a57}
body{
margin:0 ;
max-width:560px ;
padding:48px 16px ;
margin-inline:auto ;
font:15px/1.5 system-ui ;
background:#000 ;
color:#f2f2f2 ;
}

h1{margin:4px 0 ;font:700 clamp(36px,8vw,42px)/1 system-ui}
p{margin:0 ;color:#999}
header{padding-bottom:16px ;border-bottom:1px solid var(--e)}
section{padding-top:16px}
nav{display:flex ;gap:8px;flex-wrap:wrap;margin-top:12px}
a{
padding:6px 10px ;
border:1px solid var(--e) ;
border-radius:6px ;
color:inherit ;
text-decoration:none ;
}

a:first-child{
background:var(--a) ;
border-color:var(--a) ;
color:#000 ;
}

"

fn asset(str body, str ctype) dict {
   web.response(body, 200, {"content-type": ctype, "cache-control": "no-cache"})
}

fn app(dict req) dict {
   def method = req.get("method", "GET")
   if method != "GET" && method != "HEAD" { return web.method_not_allowed("GET, HEAD") }
   def path = req.get("path", "/")
   if path == "/" || path == "/index.html" { return asset(HTML, "text/html; charset=utf-8") }
   if path == "/style.css" { return asset(CSS, "text/css; charset=utf-8") }
   if path == "/plain" { return web.text("ny host ok\n") }
   if path == "/health" { return web.json({"ok": true, "service": "ny-server"}) }
   web.not_found("not found: " + path + "\n")
}

def result = web.serve_cli(app, {"name": "Ny Server", "port": 8080})

if !result.get("ok", false) {
   print("server failed: " + result.get("error", "unknown error"))
   exit(1)
}
