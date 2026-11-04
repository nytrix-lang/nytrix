extern int nyc_external_after_local_static;
extern int nyc_external_after_local_static_read(void);
int main(void) {
  return nyc_external_after_local_static == 37 &&
                 nyc_external_after_local_static_read() == 37
             ? 0
             : 1;
}
