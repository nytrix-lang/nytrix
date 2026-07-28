#ifndef COMPLEX_H
#define COMPLEX_H

/* Test header modeled after <complex.h> for seamless C interop testing.
   Exercises the internal C frontend (nytrix path) for:
   - object-like macros (I, _Complex_I, etc.)
   - function-like macros (CMPLX, etc.)
   - typedefs and _Complex handling
   - function declarations
   - static inline functions
   - some structs (for layout tests)
   - enums and defines
   Goal: 100% capture of macros, symbols, functions, inlines, structs
   without libclang for complex headers. */

#define complex _Complex
#define _Complex_I ((float _Complex)0.0F + (float _Complex)(1.0F * 1.0iF))
#define I _Complex_I

typedef float _Complex float_complex;
typedef double _Complex double_complex;
typedef long double _Complex long_double_complex;

/* Object-like macros */
#define COMPLEX_VERSION 1
#define CMPLX_INFINITY __builtin_inff()
#define CMPLX_NAN __builtin_nanf("")

/* Function-like macros (common in complex.h) */
#define CMPLX(x, y) ((double complex)((double)(x) + _Complex_I * (double)(y)))
#define CMPLXF(x, y) ((float complex)((float)(x) + _Complex_I * (float)(y)))
#define CMPLXL(x, y) ((long double complex)((long double)(x) + _Complex_I * (long double)(y)))

#define RE(z) __real__(z)
#define IM(z) __imag__(z)

/* Struct for alternative complex representation (tests aggregate layout) */
typedef struct {
    double re;
    double im;
} complex_struct;

/* Function declarations (symbols) */
double creal(double complex z);
double cimag(double complex z);
double cabs(double complex z);
double carg(double complex z);
double complex conj(double complex z);
double complex cproj(double complex z);

/* More complex functions */
float crealf(float complex z);
float cimagf(float complex z);
float cabsf(float complex z);

/* Inline functions (test inline capture) */
static inline double complex cadd(double complex a, double complex b) {
    return a + b;
}

static inline double complex csub(double complex a, double complex b) {
    return a - b;
}

/* Enum for complex related */
enum {
    COMPLEX_POLAR = 1,
    COMPLEX_RECT = 2
};

/* Some #defines that use previous */
#define UNIT (1.0 + 0.0 * I)
#define IMAG_UNIT (0.0 + 1.0 * I)

#endif
