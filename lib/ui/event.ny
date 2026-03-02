;; Keywords: ui window event
;; Event representation helpers for std.ui.

module std.ui.event (
   is_event, make_event, event_type, event_window, event_window_id, event_data,
   queue_push, queue_pop, queue_len
)
use std.core *
use std.ui.consts *

def _E_TAG = 0
def _E_TYPE = 1
def _E_WINDOW = 2
def _E_WINDOW_ID = 3
def _E_DATA = 4

fn is_event(ev){
   "Returns true when `ev` is a std.ui event payload."
   is_list(ev) && len(ev) > _E_DATA && get(ev, _E_TAG, "") == "std.ui.event"
}

fn make_event(kind, win, win_id=0, data=0){
   "Constructs an event payload."
   mut ev = list(6)
   ev = append(ev, "std.ui.event")
   ev = append(ev, kind)
   ev = append(ev, win)
   ev = append(ev, win_id)
   ev = append(ev, data)
   ev
}

fn event_type(ev){
   "Returns event type enum value."
   if(!is_event(ev)){ return EVENT_NONE }
   get(ev, _E_TYPE, EVENT_NONE)
}

fn event_window(ev){
   "Returns the event's associated window object."
   if(!is_event(ev)){ return 0 }
   get(ev, _E_WINDOW, 0)
}

fn event_window_id(ev){
   "Returns the event's associated window id."
   if(!is_event(ev)){ return 0 }
   get(ev, _E_WINDOW_ID, 0)
}

fn event_data(ev){
   "Returns event payload data."
   if(!is_event(ev)){ return 0 }
   get(ev, _E_DATA, 0)
}

fn queue_push(q, ev){
   "Appends event `ev` to queue `q` and returns queue."
   if(!is_list(q)){ q = list(8) }
   append(q, ev)
}

fn queue_pop(q){
   "Pops the oldest event from queue `q` (FIFO), or 0."
   if(!is_list(q)){ return 0 }
   def n = len(q)
   if(n == 0){ return 0 }
   def first = get(q, 0)
   mut i = 1
   while(i < n){
      set_idx(q, i - 1, get(q, i))
      i += 1
   }
   pop(q)
   first
}

fn queue_len(q){
   "Returns queue length."
   if(!is_list(q)){ return 0 }
   len(q)
}
