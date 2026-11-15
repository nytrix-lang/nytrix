/*
 * Fixture: parse and call a function taking a pointer to __int128.
 */
static int is_null(__int128 *value) { return value == 0; }

int main(void) { return !is_null((__int128 *)0); }
