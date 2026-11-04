int nyc_pic_external = 42;
int nyc_pic_external_fn(void) { return 23; }
extern int *nyc_pic_static_external;
int *nyc_pic_address(void);
int *nyc_pic_local_address(void);
int **nyc_pic_static_address(void);
int *nyc_pic_static_local_address(void);
int nyc_pic_call(void);
int main(void) { return *nyc_pic_address() == 42 && *nyc_pic_local_address() == 17 && *nyc_pic_static_external == 42 && **nyc_pic_static_address() == 42 && *nyc_pic_static_local_address() == 17 && nyc_pic_call() == 23 ? 0 : 1; }
