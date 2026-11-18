/*
 * C front-end regression test: static data retention.
 */
struct operations {
  int (*callback)(int);
};

extern int absent_feature(int);
static int dead_callback(int value) { return absent_feature(value); }
static const struct operations dead_operations = {dead_callback};
static int ignore_operations(const struct operations *operations) {
  (void)operations;
  return 0;
}
int ignore_dead_operations(int value) {
  (void)ignore_operations(&dead_operations);
  return value + 7;
}

static int live_callback(int value) { return value + 11; }
static const struct operations live_operations = {live_callback};
int call_live_operations(int value) { return live_operations.callback(value); }

static int asm_callback(int value) { return value + 13; }
static const struct operations asm_operations
    __attribute__((used)) = {asm_callback};
__asm__(".pushsection .data,\"aw\"\n"
        ".globl asm_operations_reference\n"
        "asm_operations_reference:\n"
        ".quad asm_operations\n"
        ".popsection\n");

static int used_callback(int value) { return value + 17; }
__attribute__((used)) static const struct operations kept_prefix_operations = {
    used_callback};
static const struct operations kept_suffix_operations
    __attribute__((used)) = {used_callback};

static int section_callback(int value) { return value + 19; }
__attribute__((
    used, section("nyc_retained_operations"))) static const struct operations
    section_operations = {section_callback};

static int alias_callback(int value) { return value + 23; }
static const struct operations alias_operations = {alias_callback};
extern const struct operations exported_operations_alias
    __attribute__((alias("alias_operations")));

static int public_callback(int value) { return value + 29; }
const struct operations public_operations = {public_callback};
