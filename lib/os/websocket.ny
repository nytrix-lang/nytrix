;; Keywords: websocket browser networking async transport
;; Small browser WebSocket lifecycle facade.
module std.os.websocket(open, state, send, receive, close)

;; This module is a browser WebSocket facade.  Native hosts return the
;; documented zero/false/no-message values because no native socket transport
;; is registered for this API.
fn open(str url) int {
   "Opens a browser WebSocket and returns a handle, or 0 on failure."
   __websocket_open(url)
}

fn state(int handle) str {
   "Returns CONNECTING, OPEN, CLOSING, CLOSED, ERROR, or 0 on native hosts."
   __websocket_state(handle)
}

fn send(int handle, str text) bool {
   "Queues text for an open browser WebSocket."
   __websocket_send(handle, text) != 0
}

fn receive(int handle) str {
   "Returns the next text message, or 0 when no message is queued."
   __websocket_receive(handle)
}

fn close(int handle) bool {
   "Closes a browser WebSocket handle."
   __websocket_close(handle) != 0
}
