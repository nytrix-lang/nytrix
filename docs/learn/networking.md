# Networking

`std.os.net` is the facade for HTTP, sockets, local servers, process tubes,
SSH-style workflows, TLS transport, transcripts, and shared network context.

Pick the highest-level API that still exposes the data you need. `net.request`
covers HTTP, server helpers cover local fixtures, tubes cover interactive
processes or services, and raw sockets cover direct protocol implementations.

## Layers

| Layer | Entry point | Result |
| --- | --- | --- |
| HTTP client | `net.request` | Response dictionary with status, headers, body, transport, error metadata. |
| Local HTTP server | `std.os.net.server` | Blocking HTTP/1.1 handler loop for tools and fixtures. |
| Tube interaction | `remote`, `process`, `shell`, `ssh` | Buffered send/receive API with transcripts. |
| Raw socket | `socket_connect`, `socket_bind`, `socket_accept` | Direct protocol-level TCP IO. |
| Context | `net.context` | Log level, timeout, chunk size, color behavior. |

## Context

Network APIs share context values:

```ny
use std.os.net as net

net.context({
   "log_level": "debug",
   "timeout_ms": 3000,
   "chunk_size": 4096,
   "color": false
})
```

Log levels are `quiet`, `error`, `info`, `debug`, and `trace`.

## HTTP requests

`net.request(options)` or `net.request(method, url, data, headers, options)`
prepares a request and returns a response dictionary.

| Option | Meaning |
| --- | --- |
| `method` | HTTP method. Defaults to `GET` unless a body implies `POST`. |
| `url` / `uri` | Full URL. |
| `params` / `query` | Query string pairs, dict, list, or raw query string. |
| `headers` | Request headers. |
| `json` | JSON body and JSON content type. |
| `form` | URL-encoded form body. |
| `multipart` / `fields` / `files` | Multipart body fields and files. |
| `body` / `data` | Raw or form-like body. |
| `cookies` / `cookie` | Cookie dict or raw cookie header. |
| `auth` / `bearer` | Basic or bearer authorization helpers. |
| `user_agent`, `accept`, `referer` | Header shortcuts. |
| `timeout` / `timeout_sec` | Request timeout in seconds. |
| `follow` | Redirect policy. |
| `proxy` | Proxy configuration. |
| `transport`, `curl` | Transport selection and curl options. |

Response fields:

| Field | Meaning |
| --- | --- |
| `ok` | Boolean success status. |
| `status` | Numeric HTTP status. |
| `reason` | Status reason. |
| `headers` | Parsed response headers. |
| `raw_headers` | Raw header text. |
| `body` | Decoded body string/value where available. |
| `raw` | Raw response body. |
| `method` | Final method. |
| `url` | Final URL. |
| `transport` | Transport used. |
| `request` | Prepared request metadata. |
| `error` | Error text when a request fails. |

HTTPS uses curl transport when libcurl is available. Plain HTTP can use the
socket transport.

## Local HTTP server

`std.os.net.server` handlers receive a request dictionary and return a response
dictionary or helper result.

| Item | Behavior |
| --- | --- |
| Request keys | `method`, `path`, `query`, `query_params`, `headers`, `body`. |
| Response helpers | `web.text`, `web.html`, `web.json`, `web.redirect`, `web.not_found`, `web.response`. |
| Header lookup | `web.header(headers, name, fallback)` is case-insensitive. |
| CLI server | `web.serve_cli(app, {"port": 8080})` binds a local server and logs requests. |

Default no-argument examples use port `8080`.

## Tubes

Tubes provide buffered interaction with processes, TCP connections, and
SSH-launched commands.

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

| Call | Behavior |
| --- | --- |
| `remote(host, port, opts)` | Connect to a TCP service. |
| `connect_retry(host, port, opts)` | Retry connection until timeout/limit. |
| `process(path, args, opts)` | Spawn a local process as a tube. |
| `shell(command, opts)` | Spawn a shell command as a tube. |
| `ssh(opts)` | Open an SSH-style connection handle. |
| `ssh_process(ssh, command, opts)` | Run a command through SSH as a tube. |
| `send(io, data)` | Write bytes/text. |
| `sendline(io, data)` | Write data plus newline. |
| `recv(io, n)` | Read up to `n` bytes. |
| `recvuntil(io, needle)` | Read through a delimiter and buffer extra bytes. |
| `recvall(io)` | Read until EOF or configured limit. |
| `sendlineafter(io, needle, data)` | Receive delimiter, then send a line. |
| `expect(io, patterns)` | Match expected output patterns. |
| `clean(io)` | Return buffered data without waiting for more. |
| `transcript_text(io)` | Return ordered send/receive transcript. |
| `interactive(io)` | Attach the terminal to the tube. |
| `close(io)` | Close the tube. |

## Raw sockets

Raw socket helpers cover direct TCP work:

| Helper | Purpose |
| --- | --- |
| `socket_connect(host, port)` | Open a TCP connection. |
| `socket_bind(host, port)` | Bind a listening socket. |
| `socket_accept(fd)` | Accept one incoming connection. |
| `read_socket(fd, n)` | Read up to `n` bytes. |
| `read_socket_until(fd, needle)` | Read until a delimiter or limit. |
| `write_socket_all(fd, data)` | Write the full buffer. |
| `write_socket_line(fd, data)` | Write data plus newline. |
| `socket_set_timeout_ms(fd, ms)` | Set socket timeout. |
| `close_socket(fd)` | Close the socket fd. |

## Determinism

Network checks use explicit timeouts. Local servers, process tubes, readiness
checks, and transcript output make network behavior repeatable.

Prefer local services in tests. Public internet calls are useful examples only
when the text names the dependency and the code records timeout and error
metadata.

## Failure data

For failing network checks, include:

- context values: timeout, log level, chunk size
- request metadata: method, URL, transport, status, error
- tube transcript text
- local fixture command or server route
