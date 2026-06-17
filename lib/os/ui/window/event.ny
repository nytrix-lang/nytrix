;; Keywords: window event os ui input
;; Window event record construction and inspection for native input backends.
;; References:
;; - std.os.ui.window
;; - std.os.ui.window.consts
module std.os.ui.window.event(is_event, make_event, event_type, event_window, event_window_id, event_data, queue_push, queue_pop, queue_len)
use std.core

def _EVENT_MOUSE_POS_CHANGED = 7
def _E_TAG = 0
def _E_TYPE = 1
def _E_WINDOW = 2
def _E_WINDOW_ID = 3
def _E_DATA = 4
def _EVENT_NONE = 0

fn is_event(any ev) bool {
   "Returns true when `ev` is a std.os.ui event payload."
   is_list(ev) && ev.len >= 5 && ev.get(_E_TAG, "") == "std.os.ui.window.event"
}

fn make_event(i32 kind, any win, i32 win_id=0, any data=0) list {
   "Constructs an event payload."
   ["std.os.ui.window.event", kind, win, win_id, data]
}

fn _event_get(any ev, i32 slot, any fallback=0) any {
   if !is_event(ev) { return fallback }
   ev.get(slot, fallback)
}

@inline
fn event_type(any ev) int {
   "Returns event type enum value."
   if !is_event(ev) { return _EVENT_NONE }
   int(ev.get(_E_TYPE, _EVENT_NONE))
}

fn event_window(any ev) any {
   "Returns the event's associated window object."
   _event_get(ev, _E_WINDOW, 0)
}

@inline
fn event_window_id(any ev) int {
   "Returns the event's associated window id."
   if !is_event(ev) { return 0 }
   int(ev.get(_E_WINDOW_ID, 0))
}

fn event_data(any ev) any {
   "Returns event payload data."
   _event_get(ev, _E_DATA, 0)
}

fn _queue_can_coalesce_tail_motion(any q, any ev) bool {
   if !is_list(q) || q.len <= 0 || !is_event(ev) { return false }
   if event_type(ev) != _EVENT_MOUSE_POS_CHANGED { return false }
   def last = q.get(q.len - 1, [])
   if !is_event(last) || event_type(last) != _EVENT_MOUSE_POS_CHANGED { return false }
   def last_id = event_window_id(last)
   def next_id = event_window_id(ev)
   if last_id != 0 && next_id != 0 { return last_id == next_id }
   true
}

fn queue_push(any q, any ev) list {
   "Appends an event to the queue and coalesces consecutive mouse-motion events."
   if !is_list(q) { q = [] }
   if _queue_can_coalesce_tail_motion(q, ev) {
      q[q.len - 1] = ev
      return q
   }
   q.append(ev)
}

fn queue_pop(any q) any {
   "Pops the oldest event from queue `q` (FIFO). Returns oldest event or 0 if empty."
   if !is_list(q) || q.len == 0 { return 0 }
   def head = q.get(0)
   mut i = 1
   while i < q.len {
      q[i - 1] = q.get(i)
      i += 1
   }
   q.pop()
   head
}

fn queue_len(any q) i32 {
   "Returns queue length."
   if !is_list(q) { return 0 }
   q.len
}
