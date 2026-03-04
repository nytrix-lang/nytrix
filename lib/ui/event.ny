;; lib/ui/event.ny — compatibility alias
;; Moved to std.ui.window.event. This stub re-exports everything.
module std.ui.event (
   is_event, make_event, event_type, event_window, event_window_id, event_data,
   queue_push, queue_pop, queue_len
)
use std.ui.window.event *
