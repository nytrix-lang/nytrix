<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":120,"summary":"Choose an HTTP, socket, server, or tube API and handle host-specific failures."} -->
# Networking

Choose the highest-level API that still exposes the data your program needs.
The standard library provides HTTP requests, local servers, process/network
tubes, and raw sockets. Browser networking uses a separate host contract.

## Choose a layer

| Need | Starting point | Notes |
| --- | --- | --- |
| HTTP client | `core.os.net` | Use request options and inspect status, headers, body, and error. |
| Local HTTP server | `core.os.net.server` | Useful for local tools and deterministic tests. |
| Interactive process or service | `core.os.net` tube APIs | Use timeouts and retain the transcript when diagnosing a failure. |
| Direct TCP protocol | `core.os.net.socket` | The caller owns connection, framing, timeout, and close behavior. |
| Browser fetch or WebSocket | Browser host APIs | Permissions, origin policy, and server availability apply. See [Browser](browser.md). |

Find current function names and signatures in the checked-out library:

```bash
ny doc search --symbols request
ny doc search --symbols socket_connect
ny doc get core.os.net
```

## Failure handling

Network operations can fail because a name does not resolve, a connection is
refused, a remote peer closes early, a timeout expires, or a browser denies a
capability. Check the API's returned status or error value; do not treat a
nonempty response as proof of success.

Set an explicit timeout for external services. Prefer a local server or fake
peer in tests, and make the test assert the response status and body. Record the
request metadata or tube transcript when reproducing failures.

## Portability

Native sockets and process APIs are not automatically available in a browser
build. Browser `fetch` and WebSocket operations also depend on browser policy
and user permissions. Use [Execution routes](routes.md) and
[Implementation status](status.md) to check the target you intend to ship.
