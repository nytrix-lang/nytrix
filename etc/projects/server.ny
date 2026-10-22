#!/usr/bin/env ny
;; Keywords: os http server routes html json example
;; Small HTTP server with HTML, CSS, text, and JSON routes.
use core
use core.common (env_int_clamped)
use core.os (exit)
use core.os.net.server as web

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
<p>Served by <code>etc/projects/server.ny</code></p>
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
border-radius:1px ;
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

fn app(any req) dict {
   if !is_dict(req) { return web.text("invalid request shape\n", 500) }
   def str method = str(req.get("method", "GET"))
   if method != "GET" && method != "HEAD" { return web.method_not_allowed("GET, HEAD") }
   def str path = str(req.get("path", "/"))
   if path == "/" || path == "/index.html" { return asset(HTML, "text/html; charset=utf-8") }
   if path == "/style.css" { return asset(CSS, "text/css; charset=utf-8") }
   if path == "/favicon.ico" { return web.response("", 204) }
   if path == "/plain" { return web.text("ny host ok\n") }
   if path == "/health" {
      mut health = dict(2)
      health = dict_set(health, "ok", true)
      health = dict_set(health, "service", "ny-server")
      return web.json(health)
   }
   web.not_found("not found: " + path + "\n")
}

;; A fixture may cap accepted requests, the wait for each request, and the
;; whole run. The server banner and readiness marker are flushed after bind
;; succeeds.
mut options = dict(6)
options = dict_set(options, "name", "Ny Server")
options = dict_set(options, "port", 8080)
options = dict_set(options, "log", false)
options = dict_set(options, "max_requests", env_int_clamped("NYTRIX_FIXTURE_REQUESTS", -1, -1, 1000000))
options = dict_set(options, "timeout_ms", env_int_clamped("NYTRIX_FIXTURE_TIMEOUT_MS", 0, 0, 3600000))
options = dict_set(options, "deadline_ms", env_int_clamped("NYTRIX_FIXTURE_DEADLINE_MS", 0, 0, 3600000))
def result = web.serve_cli(app, options)

if !result.get("ok", false) {
   def str error = str(result.get("error", "unknown error"))
   print("server failed: " + error)
   __print_flush()
   exit(1)
}
exit(0)
