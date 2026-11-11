int nyc_weak_override(void);
int nyc_weak_alias(void);
extern int nyc_weak_data;

int main(void) {
  return nyc_weak_override() != 2 || nyc_weak_data != 2 ||
         nyc_weak_alias() != 4;
}
