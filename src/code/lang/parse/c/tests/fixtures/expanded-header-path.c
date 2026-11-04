#define NYC_PATH trace/events
#define NYC_STRINGIFY_1(value) #value
#define NYC_STRINGIFY(value) NYC_STRINGIFY_1(value)
#define NYC_INCLUDE(file) NYC_STRINGIFY(NYC_PATH/file.h)
#include NYC_INCLUDE(variadic)
int expanded_header_path = NYC_TRACE_PATH_INCLUDED;
