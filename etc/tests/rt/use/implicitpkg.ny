module implicitpkg(
   child,
   parent_value,
   parent_child_sum,
)

fn parent_value() int { 7 }

fn parent_child_sum() int { child_value() + child.child_value() }
