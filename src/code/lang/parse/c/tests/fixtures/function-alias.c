static int alias_target(void) { return 73; }
extern int alias_entry(void) __attribute__((__alias__("alias_target")));
int main(void) { return alias_entry() != 73; }
