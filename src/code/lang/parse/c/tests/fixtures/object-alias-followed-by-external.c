/*
 * C parser fixture for an object alias followed by another external object.
 */
int alias_target = 73;
extern int alias_value __attribute__((alias("alias_target")));
extern unsigned long unrelated_external;

int main(void) { return alias_value != 73; }
