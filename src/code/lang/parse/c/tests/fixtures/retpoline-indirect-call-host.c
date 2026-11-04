long retpoline_target(long);
long retpoline_call(long (*)(long), long);
int main(void) { return retpoline_call(retpoline_target, 9) != 28; }
