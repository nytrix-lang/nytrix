module implicitpkg.child(
   grand,
   child_value,
   child_label,
   child_grand_sum,
)

fn child_value(): int { 42 }

fn child_label(): str { "implicit-child" }

fn child_grand_sum(): int { child_value() + grand.grand_value() }
