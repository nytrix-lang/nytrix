/*
 * Fixture: apply integer-cast width before using a constant array extent.
 */
typedef unsigned char u8;
#define U8_MAX ((u8)~0U)

static int values[U8_MAX + 1];

int main(void) { return sizeof(values) / sizeof(values[0]) == 256 ? 0 : 1; }
