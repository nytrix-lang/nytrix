/*
 * C parser fixture for _Float32, _Float64, and _Float32x operations.
 */
_Float32 f32(_Float32 x) { return x + 0.5f; }
_Float64 f64(_Float64 x) { return x + 0.25; }
_Float32x f32x(_Float32x x) { return x + 0.125; }
