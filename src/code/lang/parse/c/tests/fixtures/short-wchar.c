/*
 * C parser fixture for a 16-bit wchar_t target data model.
 */
typedef __WCHAR_TYPE__ wchar_t;
_Static_assert(sizeof(wchar_t) == 2, "short wchar_t size");
_Static_assert(__SIZEOF_WCHAR_T__ == 2, "short wchar_t macro size");
_Static_assert(__WCHAR_WIDTH__ == 16, "short wchar_t width");
_Static_assert(__WCHAR_MAX__ == 65535, "short wchar_t maximum");
_Static_assert(__WCHAR_MIN__ == 0, "short wchar_t minimum");
int nyc_short_wchar(void) { return sizeof(wchar_t); }
