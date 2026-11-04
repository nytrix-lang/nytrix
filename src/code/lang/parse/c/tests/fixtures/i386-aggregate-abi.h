struct nyc_i386_tiny {
  unsigned char a, b, c;
};

struct nyc_i386_mixed {
  int value;
  double real;
  unsigned char tag;
};

struct nyc_i386_large {
  int values[7];
};

struct nyc_i386_real {
  double value;
};

typedef int (*nyc_i386_callback_fn)(int, int, int, int);
int nyc_i386_registers(int a, int b, int c, int d);
int host_i386_registers(int a, int b, int c, int d);
int nyc_i386_callback(nyc_i386_callback_fn callback);
int host_i386_callback(nyc_i386_callback_fn callback);
long long nyc_i386_wide(int prefix, long long value, int suffix);
long long host_i386_wide(int prefix, long long value, int suffix);
int nyc_i386_real(struct nyc_i386_real value, int a, int b, int c);
int host_i386_real(struct nyc_i386_real value, int a, int b, int c);
int host_i386_sizes(int marker, ...);
__attribute__((regparm(0))) int nyc_i386_cdecl(int a, int b, int c, int d);
__attribute__((regparm(0))) int host_i386_cdecl(int a, int b, int c, int d);

struct nyc_i386_tiny nyc_i386_tiny(int value);
struct nyc_i386_mixed nyc_i386_mix(int prefix, struct nyc_i386_tiny tiny,
                                   double scale, struct nyc_i386_mixed input,
                                   int suffix);
struct nyc_i386_large nyc_i386_large(struct nyc_i386_large input, int delta);
struct nyc_i386_tiny host_i386_tiny(struct nyc_i386_tiny input, int delta);
struct nyc_i386_mixed host_i386_mix(int prefix, struct nyc_i386_tiny tiny,
                                    double scale, struct nyc_i386_mixed input,
                                    int suffix);
int nyc_i386_verify(void);
