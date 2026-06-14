;; Keywords: platform window backend opengl nsgl os ui input
;; NSGL context creation for the Cocoa window backend.
;; References:
;; - std.os.ui.window.platform.opengl
;; - std.os.ui.window
;; - std.os.ui.window.consts
module std.os.ui.window.platform.opengl.nsgl(create_context, destroy_context, make_current, swap_buffers, swap_interval, get_proc_address)
use std.core
use std.os.ui.window.platform.cocoa as cocoa

fn create_context(any share=0) any {
   "Creates a native NSOpenGLContext for Cocoa."
   def fmt_cls = cocoa.get_class("NSOpenGLPixelFormat")
   def ctx_cls = cocoa.get_class("NSOpenGLContext")
   if !fmt_cls || !ctx_cls { return 0 }
   def attrs = zalloc(40)
   store32(attrs, 73, 0)
   store32(attrs, 5, 4)
   store32(attrs, 8, 8)
   store32(attrs, 24, 12)
   store32(attrs, 24, 16)
   store32(attrs, 0, 20)
   store32(attrs, 0, 24)
   mut fmt = cocoa.objc_msgSend_ptr(fmt_cls, cocoa.get_selector("alloc"), 0)
   fmt = cocoa.objc_msgSend_ptr(fmt, cocoa.get_selector("initWithAttributes:"), attrs)
   free(attrs)
   if !fmt { return 0 }
   mut ctx = cocoa.objc_msgSend_ptr(ctx_cls, cocoa.get_selector("alloc"), 0)
   ctx = cocoa.objc_msgSend_ptr_ptr(ctx, cocoa.get_selector("initWithFormat:shareContext:"), fmt, share)
   cocoa.objc_msgSend(fmt, cocoa.get_selector("release"))
   ctx
}

fn destroy_context(any ctx) bool {
   "Destroys destroy context."
   true
}

fn make_current(any ctx) bool {
   "Builds make current."
   if !ctx {
      def cls = cocoa.get_class("NSOpenGLContext")
      cocoa.objc_msgSend(cls, cocoa.get_selector("clearCurrentContext"))
      return true
   }
   cocoa.objc_msgSend(ctx, cocoa.get_selector("makeCurrentContext"))
   true
}

fn swap_buffers(any ctx) bool {
   "Runs the swap buffers operation."
   if !ctx { return false }
   cocoa.objc_msgSend(ctx, cocoa.get_selector("flushBuffer"))
   true
}

fn swap_interval(int interval) bool {
   "Runs the swap interval operation."
   true
}

fn get_proc_address(str name) any {
   "Returns get proc address."
   0
}
