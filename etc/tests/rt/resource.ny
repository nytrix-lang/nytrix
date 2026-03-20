use std.core
use std.core.error

mut cleanup_log = ""

fn close(str: name){
   cleanup_log = cleanup_log + name
}

fn test_with_ptr(){
   mut seen = 0
   with ptr: buf = malloc(8){
      assert(buf != 0, "with ptr binds allocation")
      store8(buf, 65, 0)
      seen = load8(buf, 0)
   }
   assert(seen == 65, "with ptr body can access resource")
}

fn test_with_cleanup(){
   cleanup_log = ""
   with str: label = "C" {
      cleanup_log = cleanup_log + "B"
   }
   assert(cleanup_log == "BC", "with runs close after body")
}

fn return_through_with(){
   with str: label = "R" {
      cleanup_log = cleanup_log + "body|"
      return 7
   }
}

fn panic_through_with(){
   try {
      with str: label = "P" {
         panic("resource boom")
      }
   } catch err {
      _ = err
   }
}

test_with_ptr()
test_with_cleanup()
cleanup_log = ""
assert(return_through_with() == 7, "with returns body value")
assert(cleanup_log == "body|R", "with cleanup runs on return")
cleanup_log = ""
panic_through_with()
assert(cleanup_log == "P", "with cleanup runs on panic")
print("✓ runtime resource tests passed")
