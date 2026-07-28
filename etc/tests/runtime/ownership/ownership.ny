use std.core

fn borrow_keeps_source_live() int {
   def a = [1, 2, 3]
   def b = borrow(a)
   assert_eq(len(b), 3, "borrowed list length")
   assert_eq(len(a), 3, "source remains usable after borrow")
   return len(a)
}

fn borrow_index_keeps_owner_live() int {
   def a = [7, 8, 9]
   def b = borrow(a[1])
   assert_eq(b, 8, "borrowed indexed value")
   assert_eq(len(a), 3, "indexed borrow keeps owner usable")
   return b
}

fn borrow_operator_keeps_source_live() int {
   def a = [21, 22]
   def b = &a
   assert_eq(len(b), 2, "borrow operator list length")
   assert_eq(len(a), 2, "borrow operator keeps source usable")
   return len(b)
}

fn release_is_explicit() int {
   def a = [4, 5]
   def r = release(a)
   assert_eq(r, 0, "release returns sentinel")
   return 2
}

@borrows(x)
@returns_borrow(x)
fn contract_peek(x) {
   x
}

@returns_owned
@consumes(x)
fn contract_adopt(x) {
   x
}

@consumes(x)
@releases(x)
fn contract_release(x) int {
   __drop_owned(x)
   0
}

@consumes(x)
@forgets(x)
fn contract_forget(x) int {
   0
}

fn contract_borrow_keeps_source_live() int {
   def a = [10, 11]
   def b = contract_peek(a)
   assert_eq(len(a), 2, "contract borrow keeps source live")
   assert_eq(len(b), 2, "contract returned borrow is usable")
   return len(a) + len(b)
}

fn contract_adopt_moves_owner() int {
   def a = [12, 13, 14]
   def b = contract_adopt(a)
   assert_eq(len(b), 3, "contract adopt returns owned value")
   return len(b)
}

fn contract_release_is_explicit() int {
   def a = [15]
   assert_eq(contract_release(a), 0, "contract release returns sentinel")
   return 1
}

fn contract_forget_is_explicit() int {
   def a = [16]
   assert_eq(contract_forget(a), 0, "contract forget returns sentinel")
   return 1
}

@returns_owned
fn returned_append_list() list {
   mut out = []
   out = out.append(1)
   out = out.append(2)
   out = out.append(3)
   out
}

assert_eq(borrow_keeps_source_live(), 3, "borrow helper preserves source")
assert_eq(borrow_index_keeps_owner_live(), 8, "indexed borrow helper preserves source")
assert_eq(borrow_operator_keeps_source_live(), 2, "borrow operator preserves source")
assert_eq(release_is_explicit(), 2, "release helper returns value")
assert_eq(contract_borrow_keeps_source_live(), 4, "contract borrow helper preserves source")
assert_eq(contract_adopt_moves_owner(), 3, "contract consume helper moves source")
assert_eq(contract_release_is_explicit(), 1, "contract release helper returns value")
assert_eq(contract_forget_is_explicit(), 1, "contract forget helper returns value")
assert_eq(to_str(returned_append_list()), "[1, 2, 3]", "returned appended list remains live")
print("✓ ownership helpers test passed")
