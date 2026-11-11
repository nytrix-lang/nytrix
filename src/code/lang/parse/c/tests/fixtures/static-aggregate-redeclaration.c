/*
 * C parser fixture for compatible declarations of a static aggregate type.
 */
struct nyc_redeclared_aggregate {
  int first;
  int second;
};

static struct nyc_redeclared_aggregate nyc_redeclared_value = {
    .first = 17,
    .second = 25,
};
static struct nyc_redeclared_aggregate nyc_redeclared_value;

int main(void) {
  return nyc_redeclared_value.first != 17 || nyc_redeclared_value.second != 25;
}
