<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":145,"summary":"Use vector types and accelerator annotations where the selected target supports them."} -->
# SIMD

Use typed vectors for data-parallel values. Keep a scalar implementation when
the program must run on targets without the requested vector capability.

## Vector values

The language exposes vector types such as `vec4<f64>` and `vec8<f32>` where
the selected target and lowering path support them. Treat a vector operation as
a typed API contract, not an automatic performance guarantee.

`std.math.vector` owns the portable vector surface:

```ny
use std.core
use std.math.vector as vec

def a = vec.vec3(1.0, 2.0, 3.0)
def b = vec.vec3(4.0, 5.0, 6.0)

assert(vec.dot(a, b) == 32.0, "dot product")
def c = vec.add(a, b)
assert(vec.at(c, 0) == 5.0, "component add")
```

Receiver methods mirror the module helpers: `vec4(...)`, `.add(other)`,
`.dot(other)`, and `.scale(factor)`.

## Accelerator intent

`@accel` marks code for the validated accelerator-lowering path.
`@accel(spirv)` requests the SPIR-V target. Unsupported target shapes report a
diagnostic; they do not silently run a different backend.

Use the library reference to select available operations:

```bash
ny doc get std.math.vector
ny doc get std.os.gpu
```

## Validate the selected path

```bash
ny --strict-types file.ny
./make optcheck
```

Measure a complete workload and check its result. Assembly output alone does
not prove vector execution or a speed improvement.

## Keep a scalar fallback

Write the scalar path first, then add a vector path guarded by a capability
check:

```ny
use std.core
use std.math.vector as vec

fn magnitude(vec.vec3 v) f64 {
   vec.length3(v)
}

fn scalar_magnitude(list v) f64 {
   def x = v[0]
   def y = v[1]
   def z = v[2]
   sqrt(x * x + y * y + z * z)
}
```

A program that needs both paths keeps them explicit and checks which one the
target selected instead of assuming a vector result.

## Related

- [Functions](../spec/functions.md#attributes)
- [Performance](performance.md)
- [Native compilation](native.md)