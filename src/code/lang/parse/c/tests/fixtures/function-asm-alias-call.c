/*
 * C parser fixture for calling a function through an assembly symbol alias.
 */
extern int nyc_alias_target(int) __asm__("nyc_actual_symbol");
int nyc_alias_caller(void) { return nyc_alias_target(1); }
