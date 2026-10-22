module project.math(add, read_resource, read_offset) {
   fn _initial_offset() int { 42 }
   def offset = _initial_offset()
   fn read_offset() int { offset }
   fn add(int left, int right) int {
      left + right
   }
   @effects(cap(read))
   fn read_resource() int {
      7
   }
}

module project.random(check) {
   fn check() bool {
      def value = #run {
         __srand(123)
         def int draw = __rand64()
         return draw % 1000 == 771
      }
      return value
   }
}

assert(project.math.add(20, 22) == 42,
   "cold and cached module interfaces preserve qualified exports")
assert(project.math.read_offset() == 42,
   "module artifacts retain private helpers used by global initializers")
assert(project.random.check(),
   "serial and parallel module workers preserve seeded comptime output")

@effects(cap("project.math.read"))
fn forward_read() int {
   project.math.read_resource()
}

assert(forward_read() == 7,
   "cold and cached module interfaces preserve custom effect rows")
