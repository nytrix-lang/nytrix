use std.core *
use std.core.test *

if(__is_main_script()){
    ;; Control Flow (Test)
    ;; Tests conditional logic including if, elif, and else statements, along with nesting.

    mut x = 1
    if x == 1 {
       assert(true, "if works")
    } else {
       assert(false, "else failed")
    }

    x = 2
    if x == 1 {
       assert(false, "if failed")
    } elif x == 2 {
       assert(true, "elif works")
    } else {
       assert(false, "else failed")
    }

    x = 3
    if x == 1 {
       assert(false, "if failed")
    } elif x == 2 {
       assert(false, "elif 1 failed")
    } elif x == 3 {
       assert(true, "elif 2 works")
    } else {
       assert(false, "else failed")
    }

    x = 4
    if x == 1 {
       assert(false, "if failed")
    } elif x == 2 {
       assert(false, "elif 1 failed")
    } else {
       assert(true, "else works after elif")
    }

    ; Combined if/elif/else with nesting
    if true {
       if false {
          assert(false, "inner if failed")
       } elif true {
          assert(true, "inner elif works")
       }
    }

    print("✓ std.core.control tests passed")
}
