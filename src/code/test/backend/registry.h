/* Public test-runner interface for compiler-linked backend tests. */
#ifndef NYR_BACKEND_TEST_H
#define NYR_BACKEND_TEST_H

/* Run all compiler-linked backend regression tests, or select by relative
 * backend test path (for example `passes/scalar/cse`) or unique basename. */
int nyr_backend_test_run(const char *name);

#endif
