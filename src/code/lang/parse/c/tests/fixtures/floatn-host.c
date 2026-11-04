extern float f32(float); extern double f64(double), f32x(double);
int main(void) { return f32(1.0f) != 1.5f || f64(1.0) != 1.25 || f32x(1.0) != 1.125; }
