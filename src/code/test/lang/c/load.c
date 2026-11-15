/*
 * Parser fixture: load a signed integer through a pointer parameter.
 */
long c_load(const long *pointer) { return *pointer; }
