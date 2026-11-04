int nyc_no_sse_sum(const int *, int);
int main(void) { int values[] = { 7, -3, 11, 5 }; return nyc_no_sse_sum(values, 4) != 20; }
