/*
 * C parser fixture for typeof of a dereferenced function-pointer type.
 */
struct cred;
typedef typeof(*((int (*)(const struct cred *))((void *)0))) callback_t;

callback_t callback;

int main(void) { return 0; }
