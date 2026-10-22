<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":145,"summary":"Use vector operations only where the chosen target supports them and measurement justifies them."} -->
# SIMD and acceleration

Start with a correct scalar implementation. Use a vector API or accelerator
annotation only when the selected target supports the operation and a measured
workload benefits from it. A vector type or annotation is not a performance
guarantee.

`core.math.vector` documents vector constructors and operations. Use `ny doc`
to find the current signatures:

```bash
ny doc get core.math.vector
ny doc get core.os.gpu
```

The compiler exposes target-specific vector and accelerator forms, including
`@accel` and `@accel(spirv)`. Unsupported shapes must produce a diagnostic;
check the exact target in [Native](../spec/native.md) and the attribute contract
in [Functions](../spec/functions.md#attributes).

Measure the complete workload with the same input and result check before and
after adding a vector path. Inspect compiler output to understand lowering, but
do not use assembly presence alone as evidence of speed. See
[Performance](performance.md).
