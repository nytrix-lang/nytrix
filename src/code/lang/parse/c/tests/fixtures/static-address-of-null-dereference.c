/*
 * C parser fixture for static addresses involving a null-base member access.
 */
struct sysrq_operation;

#define NULL_OBJECT (*(const struct sysrq_operation *)((void *)0))

static const struct sysrq_operation *const operations[] = {
    &NULL_OBJECT,
    0,
};

int main(void) { return operations[0] != 0 || operations[1] != 0; }
