use core.compiler.codegen (table_gen)

fn semantic_effect_io() {
   print("artifact runtime call must not execute")
}

fn semantic_effect_pure(i32 value) i32 {
   return value
}

fn semantic_effect_caller(i32 value) i32 {
   semantic_effect_io()
   return semantic_effect_pure(value)
}

fn semantic_effect_callback(i32 value) i32 {
   print("artifact callback")
   return value
}

fn semantic_effect_hof(list values) any {
   return values.map(semantic_effect_callback)
}

fn semantic_effect_local_alias(i32 value) i32 {
   def callback = semantic_effect_callback
   return callback(value)
}

fn semantic_effect_value() i32 {
   semantic_effect_io()
   return 1
}

fn semantic_effect_enclosing(i32 value) i32 {
   return value + semantic_effect_value()
}

@effects(all)
fn semantic_effect_unknown_parent(fnptr callback) i32 {
   return 1 + callback()
}

fn semantic_effect_match(bool selected) i32 {
   match selected {
      true -> semantic_effect_value()
      _ -> 0
   }
}

fn semantic_effect_match_expression(bool selected) i32 {
   return match selected {
      true -> semantic_effect_value()
      _ -> 0
   }
}

fn semantic_effect_ternary_expression(bool selected) i32 {
   return selected ? semantic_effect_value() : 0
}

@effects(all)
fn semantic_effect_dynamic(fnptr callback) i32 {
   return callback()
}

fn semantic_requires_target(int value) int requires value >= 0 {
   return value
}

fn semantic_requires_caller(int x) int requires x >= 0 {
   return semantic_requires_target(x)
}

@total
fn semantic_decreases_countdown(int value) int requires value >= 0 decreases value {
   if value == 0 { return 0 }
   return semantic_decreases_countdown(value - 1)
}

layout SemanticArtifactLayout {
   i32 value
   bool ready
}

module SemanticOwnerLeft {
   layout SharedArtifactLayout {
      i32 left_value
   }
}

module SemanticOwnerRight {
   layout SharedArtifactLayout {
      i32 right_value
   }
}

comptime emit table_gen(SemanticArtifactLayout)

comptime template semantic_artifact_generated() {
   fn semantic_artifact_generated_fn(int value) int {
      return value + 1
   }
}

comptime emit semantic_artifact_generated()

fn semantic_effect_if_init() i32 {
   if (semantic_effect_value() > 0) {
      return 1
   }
   return 0
}

fn semantic_effect_while_init() i32 {
   while (semantic_effect_value() > 1) {
      break
   }
   return 0
}

fn semantic_effect_comptime_value() i32 {
   return comptime{1}
}

fn semantic_effect_sizeof_value() int {
   return sizeof(semantic_effect_value())
}

fn semantic_effect_quoted_syntax() i32 {
   def quoted = quote{ semantic_effect_value() }
   return 1
}

module project.storage(read_resource) {
   @effects(cap(read))
   fn read_resource() i32 {
      1
   }
}

module project.audit(log_event) {
   @effects(cap(write))
   fn log_event() i32 {
      2
   }
}

@effects(cap("project.storage.read"), cap("project.audit.write"))
fn semantic_effect_custom_parent() i32 {
   return 1 + project.storage.read_resource() + project.audit.log_event()
}

@effects(cap("project.storage.read"))
fn semantic_effect_custom_callback(i32 value) i32 {
   return value
}

@effects(cap("project.storage.read"))
fn semantic_effect_dynamic_storage_callback() i32 {
   return 11
}

@effects(cap("project.audit.write"))
fn semantic_effect_dynamic_audit_callback() i32 {
   return 12
}

@effects(cap("project.storage.read"), alloc, read_memory)
fn semantic_effect_custom_hof(list values) any {
   return values.map(semantic_effect_custom_callback)
}

@effects(alloc, read_memory, cap("project.storage.read"), e)
fn semantic_effect_custom_container_factory(fnptr callback) fnptr {
   def callbacks = {"run": callback}
   def alias = callbacks
   def extracted = alias.get("run")
   def captured = extracted
   return fn() { captured() }
}

@effects(alloc, read_memory, cap("project.storage.read"))
fn semantic_effect_custom_container_caller() i32 {
   def callback = semantic_effect_custom_container_factory(
      semantic_effect_custom_callback)
   return callback(42)
}

@effects(alloc, read_memory, e)
fn semantic_effect_custom_tuple_factory(fnptr callback) fnptr {
   def callbacks = (callback,)
   def extracted = callbacks.get(0)
   def alias = extracted
   return fn() { alias() }
}

@effects(alloc, read_memory, cap("project.storage.read"))
fn semantic_effect_custom_tuple_caller() i32 {
   def callback = semantic_effect_custom_tuple_factory(
      semantic_effect_custom_callback)
   return callback(42)
}

@effects(alloc, read_memory, cap("project.storage.read"),
   cap("project.audit.write"))
fn semantic_effect_dynamic_custom_index_caller(int index) i32 {
   def callbacks = [semantic_effect_dynamic_storage_callback,
      semantic_effect_dynamic_audit_callback]
   return callbacks[index]()
}

@effects(alloc, read_memory, cap("project.storage.read"),
   cap("project.audit.write"))
fn semantic_effect_dynamic_custom_tuple_index_caller(int index) i32 {
   def callbacks = (semantic_effect_dynamic_storage_callback,
      semantic_effect_dynamic_audit_callback)
   def captured = callbacks
   return captured[index]()
}

@effects(alloc, read_memory, cap("project.storage.read"),
   cap("project.audit.write"))
fn semantic_effect_dynamic_custom_dict_index_caller(str key) i32 {
   def callbacks = {"storage": semantic_effect_dynamic_storage_callback,
      "audit": semantic_effect_dynamic_audit_callback}
   def captured = callbacks
   def callback = captured[key]
   return callback()
}

fn lemma semantic_effect_finite(Fin<5> n) { n < 5 }

fn semantic_effect_finite_index(Fin<7> index) i32 {
   return index
}

trait SemanticLawTrait {
   fn tag(any value) int
}

struct SemanticLawProved {
   int x
}

struct SemanticLawRefuted {
   int x
}

struct SemanticLawUnknown {
   int x
}

impl SemanticLawTrait for SemanticLawProved {
   fn tag(any value) int { return 1 }
   law { 1 == 1 }
}

impl SemanticLawTrait for SemanticLawRefuted {
   fn tag(any value) int { return 2 }
   law { 1 == 2 }
}

impl SemanticLawTrait for SemanticLawUnknown {
   fn tag(any value) int { return 3 }
   law { semantic_law_probe() > 0 }
}

fn semantic_law_probe() int {
   return 1
}

fn semantic_invariant_trivial(int limit) int {
   mut n = 0
   while n < limit invariant(1 == 1) {
      n += 1
   }
   return n
}

fn semantic_invariant_symbolic(int limit) int {
   mut n = 0
   while n < limit invariant(n <= limit + 1) {
      n += 1
   }
   return n
}

refine SemanticPositive = int where(value) { value > 0 }

fn semantic_refined_local() int {
   def SemanticPositive local_value = 4
   return local_value
}
