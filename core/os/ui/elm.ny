;; Keywords: ui elm model update view messages application state
;; A compact Elm-style application loop over an explicit model and callbacks.
;; Update callbacks use `fn(any model, any message) -> any`; view callbacks use
;; `fn(any model) -> any`. Passing callbacks explicitly keeps their dynamic
;; callable ABI intact instead of boxing function pointers inside the model.
module core.os.ui.elm(new, dispatch, dispatch_many, model, view, revision, snapshot, frame, subscribe, unsubscribe, publish, publish_window_event, subscription_count, command_task, cancel_task, tick_tasks, task_count)
use core
use core.os.async as ny_async
use core.os.ui.window.event as window_event
use core.os.ui.window.consts as win

fn new(any initial_model) dict {
   "Creates application state from its initial model."
   {"model": initial_model, "revision": 0, "last_message": nil, "subscriptions": [], "next_subscription_id": 1, "tasks": [], "next_task_id": 1}
}

fn dispatch(dict app, any message, fnptr update_fn) dict {
   "Applies update(model, message) and increments revision. The callback accepts and returns any values."
   app["model"] = update_fn(app.get("model", nil), message)
   app["revision"] = int(app.get("revision", 0)) + 1
   app["last_message"] = message
   app
}

fn dispatch_many(dict app, list messages, fnptr update_fn) dict {
   "Applies messages in order to one application model."
   for message in messages { dispatch(app, message, update_fn) }
   app
}

fn model(dict app) any { "Returns the current model." app.get("model", nil) }

fn revision(dict app) int { "Returns the number of applied messages." int(app.get("revision", 0)) }

fn view(dict app, fnptr view_fn) any { "Evaluates view(model); the callback accepts and returns any values." view_fn(model(app)) }

fn snapshot(dict app, fnptr view_fn) dict {
   "Returns a render-ready model, view, revision and last-message snapshot."
   {"model": model(app), "view": view(app, view_fn), "revision": revision(app), "last_message": app.get("last_message", nil)}
}

fn frame(dict app, list messages, fnptr update_fn, fnptr view_fn) dict {
   "Applies this frame's messages, advances cooperative tasks, and returns a render-ready snapshot plus task completion status."
   dispatch_many(app, messages, update_fn)
   def task_report = tick_tasks(app, update_fn)
   mut result = snapshot(app, view_fn)
   result["tasks"] = task_report
   result
}

fn subscribe(dict app, str topic, fnptr selector) int {
   "Registers selector(event) for a topic. A nil selector result ignores that event; any other result is dispatched as an application message by publish."
   def id = int(app.get("next_subscription_id", 1))
   app["next_subscription_id"] = id + 1
   app["subscriptions"] = app.get("subscriptions", []).append({"id": id, "topic": topic, "selector": selector})
   id
}

fn unsubscribe(dict app, int subscription_id) bool {
   "Removes a subscription by the id returned from subscribe."
   mut next = []
   mut removed = false
   for sub in app.get("subscriptions", []) {
      if int(sub.get("id", 0)) == subscription_id { removed = true }
      else { next = next.append(sub) }
   }
   app["subscriptions"] = next
   removed
}

fn publish(dict app, str topic, any event, fnptr update_fn) int {
   "Routes an external event through matching subscriptions and applies each selected message in registration order."
   mut delivered = 0
   for sub in app.get("subscriptions", []) {
      if to_str(sub.get("topic", "")) != topic { continue }
      def fnptr selector = sub.get("selector")
      def message = selector(event)
      if is_nil(message) { continue }
      dispatch(app, message, update_fn)
      delivered += 1
   }
   delivered
}

fn subscription_count(dict app) int {
   "Returns the number of active event subscriptions."
   app.get("subscriptions", []).len
}

fn publish_window_event(dict app, any event, fnptr update_fn) int {
   "Publishes a polled window lifecycle event on the window.lifecycle topic. Selectors receive kind, window_id and data, and can filter by window_id. Call from the existing event loop."
   def kind = window_event.event_type(event)
   if kind != win.EVENT_WINDOW_MOVED && kind != win.EVENT_WINDOW_RESIZED &&
   kind != win.EVENT_WINDOW_REFRESH && kind != win.EVENT_WINDOW_MAXIMIZED &&
   kind != win.EVENT_WINDOW_MINIMIZED && kind != win.EVENT_WINDOW_RESTORED &&
   kind != win.EVENT_FOCUS_IN && kind != win.EVENT_FOCUS_OUT &&
   kind != win.EVENT_SCALE_UPDATED && kind != win.EVENT_QUIT { return 0 }
   def window_id = window_event.event_window_id(event)
   if window_id <= 0 { return 0 }
   publish(app, "window.lifecycle", {"kind": kind, "window_id": window_id, "data": window_event.event_data(event)}, update_fn)
}

fn _elm_task_run(any env) any {
   if env.get("cancelled", false) { return nil }
   def fnptr work = env.get("work")
   def result = work(env.get("arg", nil))
   if env.get("cancelled", false) { return nil }
   def fnptr to_message = env.get("to_message")
   to_message(result)
}

fn command_task(dict app, fnptr work, any arg, fnptr to_message) int {
   "Schedules work(arg) on the cooperative async runtime. Its mapped message is dispatched by tick_tasks when the task completes."
   def id = int(app.get("next_task_id", 1))
   app["next_task_id"] = id + 1
   def env = {"work": work, "arg": arg, "to_message": to_message, "cancelled": false}
   def handle = ny_async.Task(_elm_task_run, env)
   app["tasks"] = app.get("tasks", []).append({"id": id, "handle": handle, "env": env})
   id
}

fn cancel_task(dict app, int task_id) bool {
   "Cancels a pending command. Queued work is skipped and an undelivered result is discarded. Work already executing is allowed to return."
   mut remaining = []
   mut cancelled = false
   for task in app.get("tasks", []) {
      if int(task.get("id", 0)) == task_id {
         def dict env = task.get("env")
         env["cancelled"] = true
         cancelled = true
      } else {
         remaining = remaining.append(task)
      }
   }
   if cancelled { app["tasks"] = remaining }
   cancelled
}

fn tick_tasks(dict app, fnptr update_fn) dict {
   "Advances the cooperative async scheduler once and dispatches messages from tasks completed on this turn. Returns completed and failed task ids."
   ny_async.pump()
   mut pending = []
   mut completed = []
   mut failed = []
   for task in app.get("tasks", []) {
      if task.get("env").get("cancelled", false) { continue }
      def id = int(task.get("id", 0))
      def handle = task.get("handle", 0)
      def status = ny_async.state(handle)
      if status == 3 {
         def result = ny_async.await(handle)
         if !is_nil(result) { dispatch(app, result, update_fn) }
         completed = completed.append(id)
      } elif status == 4 || status == 5 {
         failed = failed.append(id)
      } else { pending = pending.append(task) }
   }
   app["tasks"] = pending
   {"completed": completed, "failed": failed, "pending": pending.len}
}

fn task_count(dict app) int {
   "Returns the number of pending async command tasks."
   app.get("tasks", []).len
}
