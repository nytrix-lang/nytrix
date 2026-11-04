char nyc_initdata_cmd[2] = { 'k', 0 };
char nyc_initdata_first(void);
int main(void) { return nyc_initdata_first() != 'k'; }
