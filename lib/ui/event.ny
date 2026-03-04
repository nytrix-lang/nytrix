;; Keywords: ui window event
;; Event representation helpers for std.ui.

module std.ui.event (
   is_event, make_event, event_type, event_window, event_window_id, event_data,
   queue_push, queue_pop, queue_len
)
use std.core *
use std.ui.consts *
use std.util.common as common

def _E_TAG = 0
def _E_TYPE = 1
def _E_WINDOW = 2
def _E_WINDOW_ID = 3
def _E_DATA = 4

fn is_event(ev){
   "Returns true when `ev` is a std.ui event payload."
   common.touch(ev)
   is_list(ev) && len(ev) >= 5 && get(ev, _E_TAG, "") == "std.ui.event"
}

fn make_event(kind, win, win_id=0, data=0){
   "Constructs an event payload."
   common.touch(kind) common.touch(win) common.touch(win_id) common.touch(data)
   ["std.ui.event", kind, win, win_id, data]
}

fn _event_get(ev, slot, fallback=0){
   "Internal: common getter for event list slots with validation."
   common.touch(ev)
   if(!is_event(ev)){ return fallback }
   get(ev, slot, fallback)
}

fn event_type(ev){
   "Returns event type enum value."
   _event_get(ev, _E_TYPE, EVENT_NONE)
}

fn event_window(ev){
   "Returns the event's associated window object."
   _event_get(ev, _E_WINDOW, 0)
}

fn event_window_id(ev){
   "Returns the event's associated window id."
   _event_get(ev, _E_WINDOW_ID, 0)
}

fn event_data(ev){
   "Returns event payload data."
   _event_get(ev, _E_DATA, 0)
}

fn queue_push(q, ev){
   "Appends event `ev` to queue `q` and returns queue."
   common.touch(q) common.touch(ev)
   if(!is_list(q)){ q = [] }
   append(q, ev)
}

fn queue_pop(q){
   "Pops the oldest event from queue `q` (FIFO). Returns oldest event or 0 if empty."
   common.touch(q)
   if(!is_list(q) || len(q) == 0){ return 0 }
   get(q, 0)
}

fn queue_len(q){
   "Returns queue length."
   common.touch(q)
   if(!is_list(q)){ return 0 }
   len(q)
}
