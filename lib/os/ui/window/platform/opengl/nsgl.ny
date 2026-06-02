;; Keywords: platform window backend opengl nsgl
;; NSGL context creation for the Cocoa window backend.
module std.os.ui.window.platform.opengl.nsgl(create_context, destroy_context, make_current, swap_buffers, swap_interval, get_proc_address)
use std.core
use std.os.ui.window.platform.cocoa as cocoa

fn create_context(any: share=0): any {
   "Creates a native NSOpenGLContext for Cocoa."
   def fmt_cls = cocoa.get_class("NSOpenGLPixelFormat")
   def ctx_cls = cocoa.get_class("NSOpenGLContext")
   if(!fmt_cls || !ctx_cls){ return 0 }
   def attrs = zalloc(40) ;; uint32_t attributes[]
   store32(attrs, 73, 0) ;; NSOpenGLPFAAccelerated
   store32(attrs, 5, 4) ;; NSOpenGLPFADoubleBuffer
   store32(attrs, 8, 8) ;; NSOpenGLPFAColorSize
   store32(attrs, 24, 12) ;; NSOpenGLPFAAlphaSize (using 24 for bits but usually it is components)
   store32(attrs, 24, 16) ;; NSOpenGLPFADepthSize
   store32(attrs, 0, 20) ;; NSOpenGLPFAOpenGLProfile (if needed)
   store32(attrs, 0, 24) ;; Terminating zero
   mut fmt = cocoa.objc_msgSend_ptr(fmt_cls, cocoa.get_selector("alloc"), 0)
   fmt = cocoa.objc_msgSend_ptr(fmt, cocoa.get_selector("initWithAttributes:"), attrs)
   free(attrs)
   if(!fmt){ return 0 }
   mut ctx = cocoa.objc_msgSend_ptr(ctx_cls, cocoa.get_selector("alloc"), 0)
   ctx = cocoa.objc_msgSend_ptr_ptr(ctx, cocoa.get_selector("initWithFormat:shareContext:"), fmt, share)
   cocoa.objc_msgSend(fmt, cocoa.get_selector("release"))
   ctx
}

fn destroy_context(any: ctx): bool { true }

fn make_current(any: ctx): bool {
   if(!ctx){
      def cls = cocoa.get_class("NSOpenGLContext")
      cocoa.objc_msgSend(cls, cocoa.get_selector("clearCurrentContext"))
      return true
   }
   cocoa.objc_msgSend(ctx, cocoa.get_selector("makeCurrentContext"))
   true
}

fn swap_buffers(any: ctx): bool {
   if(!ctx){ return false }
   cocoa.objc_msgSend(ctx, cocoa.get_selector("flushBuffer"))
   true
}

fn swap_interval(int: interval): bool { true }

fn get_proc_address(str: name): any { 0 }
