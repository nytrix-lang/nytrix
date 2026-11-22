use core

def legacy_value = comptime{ 2 ** 5 }
def scoped_value = #comptime{ return legacy_value + 1 }
def concise_value = #run 2 ** 5
#template make_axis(axis) {
   fn generated_axis_${axis}() { return "${axis}" }
}

for axis in #run ["x", "y"] {
   #insert make_axis(ident(axis))
}

0
