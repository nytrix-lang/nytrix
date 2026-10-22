;; Keywords: codegen reflection layout json
;; Compile-time helpers that emit ordinary Ny declarations from layout facts.
;; References:
;; - core
module core.compiler.codegen(json_for, soa_for, table_gen, match_gen, zero_for,
   load_fields_for, eq_for, hash_for,
   debug_str_for, defaults_for, new_for, store_fields_for,
   store_dict_for, new_fields_for, store_for,
   default_for, default_ctor_for)
use core

;; Allocate a zero-filled instance using the canonical layout ABI size.
;; Kept as ordinary generated code so it follows the regular type and backend
;; validation path like the legacy layout derive helper.
comptime template zero_for(Layout) {
   fn ${Layout}_zero() ptr {
      def ptr out = malloc(__layout_size("${Layout}"))
      if !out { return 0 }
      memset(out, 0, __layout_size("${Layout}"))
      return out
   }
}

;; Read all layout fields into an ordered-name dictionary through the canonical
;; layout ABI. This keeps field enumeration in Ny while values use load_layout.
comptime template load_fields_for(Layout) {
   fn ${Layout}_load_fields(ptr self) dict {
      mut dict result = dict()
      if !self { return result }
      comptime fields(Layout) as field {
         emit result = result.set(field.name,
            load_layout(self, "${Layout}", field.name))
      }
      result
   }
   comptime fields(Layout) as field {
      emit fn ${Layout}_load_${field.name}(ptr self) ${field.type} {
         return load_layout(self, "${Layout}", field.name)
      }
   }
}

;; Compare every field with the declared layout and reject null operands.
comptime template eq_for(Layout) {
   fn ${Layout}_eq(ptr a, ptr b) bool {
      if !a || !b { return false }
      comptime fields(Layout) as field {
         emit if load_layout(a, "${Layout}", field.name) !=
         load_layout(b, "${Layout}", field.name) {
            return false
         }
      }
      return true
   }
}

;; Reproduce the legacy layout hash fold over fields in declaration order.
comptime template hash_for(Layout) {
   fn ${Layout}_hash(ptr self) int {
      if !self { return 0 }
      mut h = 17
      comptime fields(Layout) as field {
         emit h = h * 31 +
         hash(load_layout(self, "${Layout}", field.name))
      }
      return h
   }
}

;; Render fields in declaration order using the legacy debug string shape.
comptime template debug_str_for(Layout) {
   fn ${Layout}_debug_str(ptr self) str {
      if !self { return "<null>" }
      mut result = "${Layout}{"
      mut first = true
      comptime fields(Layout) as field {
         emit if !first { result = result + ", " }
         emit result = result + field.name + "=" +
         to_str(load_layout(self, "${Layout}", field.name))
         emit first = false
      }
      result + "}"
   }
}

;; Return layout defaults as a field-name dictionary. The reflection API
;; supplies the declared expression or the layout constructor's scalar
;; fallback value.
comptime template defaults_for(Layout) {
   fn ${Layout}_defaults() dict {
      mut values = dict()
      comptime fields(Layout) as field {
         emit values = values.set(field.name, field.default_value)
      }
      values
   }
}

;; Build a no-argument value from each field's explicit or implicit default.
;; Reflection supplies evaluated default expressions while ordinary generated
;; stores retain compiler-owned layout/type validation.
comptime template default_for(Layout) {
   fn ${Layout}() ptr {
      def ptr destination = malloc(__layout_size("${Layout}"))
      if !destination { return 0 }
      comptime fields(Layout) as field {
         emit store_layout_field(destination, "${Layout}",
            field.name, field.default_value)
      }
      destination
   }
}

;; Generate the C derive(default) constructor shape: typed parameters with
;; reflected field defaults, followed by verified per-field layout stores.
comptime template default_ctor_for(Layout) {
   fn ${Layout}(comptime fields(Layout)) ptr {
      def ptr out = malloc(__layout_size("${Layout}"))
      if !out { return 0 }
      comptime fields(Layout) as field {
         emit store_layout_field(out, "${Layout}",
            field.name, ${field.name})
      }
      return out
   }
}

;; Allocate a layout value, starting from declared defaults and overriding
;; fields present in the input dictionary.
comptime template new_for(Layout) {
   fn ${Layout}_new(dict values) ptr {
      def ptr destination = malloc(__layout_size("${Layout}"))
      if !destination { return 0 }
      if !${Layout}_store_dict(destination, values) {
         free(destination)
         return 0
      }
      destination
   }
}

;; Generate the C derive(store) calling shape from reflected typed fields. The
;; ordinary parameter and store expressions are validated after expansion.
comptime template new_fields_for(Layout) {
   fn ${Layout}_new_fields(comptime fields(Layout)) ptr {
      def ptr destination = malloc(__layout_size("${Layout}"))
      if !destination { return 0 }
      comptime fields(Layout) as field {
         emit store_layout_field(destination, "${Layout}",
            field.name, ${field.name})
      }
      destination
   }
}

;; Generate the fixed-arity field store corresponding to the C derive(store)
;; signature. Each value is a typed parameter introduced by the field splice.
comptime template store_for(Layout) {
   fn ${Layout}_store(ptr out, comptime fields(Layout, required)) ptr {
      if !out { return 0 }
      comptime fields(Layout) as field {
         emit store_layout_field(out, "${Layout}",
            field.name, ${field.name})
      }
      return out
   }
}

;; Generate a name-checked field writer. Storage layout and primitive casts
;; remain owned by the compiler's verified layout-store intrinsic.
comptime template store_fields_for(Layout) {
   fn ${Layout}_store_field(ptr destination, str field_name, any value) bool {
      if !destination { return false }
      mut stored = false
      comptime fields(Layout) as field {
         emit if field_name == field.name {
            store_layout_field(destination, "${Layout}", field.name, value)
            stored = true
         }
      }
      stored
   }
   comptime fields(Layout) as field {
      emit fn ${Layout}_store_${field.name}(ptr destination,
         ${field.type} value) bool {
         if !destination { return false }
         store_layout_field(destination, "${Layout}", field.name, value)
         true
      }
   }
}

;; Store a named field dictionary into an existing layout allocation. Missing
;; keys receive the same effective defaults as the layout constructor.
comptime template store_dict_for(Layout) {
   fn ${Layout}_store_dict(ptr destination, dict values) ptr {
      if !destination { return 0 }
      for key in dict_keys(values) {
         mut known = false
         comptime fields(Layout) as field {
            emit if key == field.name { known = true }
         }
         if !known { return 0 }
      }
      comptime fields(Layout) as field {
         emit if values.contains(field.name) {
            store_layout_field(destination, "${Layout}", field.name,
               values.get(field.name))
         } else {
            store_layout_field(destination, "${Layout}", field.name,
               field.default_value)
         }
      }
      destination
   }
}

comptime template json_for(Layout) {
   fn ${Layout}_json_value(ptr value) dict {
      mut dict out = dict()
      if !value { return out }
      comptime fields(Layout) as field {
         emit out = out.set(field.name,
            load_layout(value, "${Layout}", field.name))
      }
      out
   }
}

comptime template soa_for(Layout) {
   fn ${Layout}_soa(list rows) dict {
      mut dict columns = dict()
      comptime fields(Layout) as field {
         emit columns = columns.set(field.name, (fn(list source) list {
                  mut values = []
                  mut i = 0
                  while i < source.len {
                     def ptr row = source[i]
                     values = values.append(row.(field))
                     i += 1
                  }
                  values
               })(rows))
      }
      columns
   }
}

comptime template table_gen(Layout) {
   fn ${Layout}_table_data(list values) dict {
      mut headers = []
      comptime fields(Layout) as field {
         emit headers = headers.append(field.name)
      }
      mut rows = []
      mut i = 0
      while i < values.len {
         def ptr row = values[i]
         mut cells = []
         comptime fields(Layout) as field {
            emit cells = cells.append(to_str(row.(field)))
         }
         rows = rows.append(cells)
         i += 1
      }
      {"headers": headers, "rows": rows}
   }
}

comptime template match_gen(Enum) {
   ;; Generate a typed name lookup without inspecting or copying payloads.
   fn ${Enum}_variant_name(Enum value) str {
      mut name = ""
      comptime variants(Enum) as variant {
         emit match value {
            variant.pattern -> { name = variant.name }
            _ -> {}
         }
      }
      name
   }
}
