extern int nyc_pic_external;
extern int nyc_pic_external_fn(void);
static int nyc_pic_local = 17;
int *nyc_pic_static_external = &nyc_pic_external;
static int *nyc_pic_static_local = &nyc_pic_local;
int *nyc_pic_address(void) { return &nyc_pic_external; }
int *nyc_pic_local_address(void) { return &nyc_pic_local; }
int **nyc_pic_static_address(void) { return &nyc_pic_static_external; }
int *nyc_pic_static_local_address(void) { return nyc_pic_static_local; }
int nyc_pic_call(void) { return nyc_pic_external_fn(); }
