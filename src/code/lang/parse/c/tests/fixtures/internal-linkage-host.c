/*
 * Runtime harness comparing separate translation units with internal linkage.
 */
int linkage_a(void);
int linkage_b(void);
int main(void) { return linkage_a() != 157 || linkage_b() != 159; }
