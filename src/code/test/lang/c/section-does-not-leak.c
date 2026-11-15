/*
 * Regression: a preceding addressable anchor's section must not attach to
 * the next file-scope character array.
 */
static int section_target;
static void *__attribute__((
    used, section(".discard.addressable"))) addressable_anchor =
    &section_target;
const char section_following_array[] = "kept";
