typedef struct Deep0 { long value; } Deep0;
typedef struct Deep1 { Deep0 value; } Deep1;
typedef struct Deep2 { Deep1 value; } Deep2;
typedef struct Deep3 { Deep2 value; } Deep3;
typedef struct Deep4 { Deep3 value; } Deep4;
typedef struct Deep5 { Deep4 value; } Deep5;
typedef struct Deep6 { Deep5 value; } Deep6;
typedef struct Deep7 { Deep6 value; } Deep7;
typedef struct Deep8 { Deep7 value; } Deep8;
typedef struct Deep9 { Deep8 value; } Deep9;

Deep9 make_deep_value(void);
long consume_deep_value(Deep9 value);
