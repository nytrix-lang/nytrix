/* -fpatchable-function-entry=N,M pads each function and -mrecord-mcount records each profiling call. */
int patchable_entry(int x) { return x + 1; }
