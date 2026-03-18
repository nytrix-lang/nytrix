;; expect: conflicting attributes '@jit' and '@async_effects'
@jit
@async_effects
fn bad_async_effects_combo(){
   1
}
