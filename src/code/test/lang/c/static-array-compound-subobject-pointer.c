/*
 * Fixture: initialize an array subobject with a pointer-valued compound
 * literal.
 */
struct device_attribute {
  unsigned mode;
  int (*show)(void);
};

struct string_attribute {
  struct device_attribute attr;
  const char *text;
};

static int show_value(void) { return 17; }

#define STRING_ATTRIBUTE(text_)                                          \
  (&((struct string_attribute[]){                                        \
      {.attr = {.mode = 0444, .show = show_value}, .text = (text_)}})[0] \
        .attr)
#define STRING_ATTRIBUTE_TEXT(text_)                                     \
  (&((struct string_attribute[]){                                        \
      {.attr = {.mode = 0444, .show = show_value}, .text = (text_)}})[0] \
        .text)

static struct device_attribute *const attributes[] = {
    STRING_ATTRIBUTE("config:0-20"),
    0,
};
static const char *const *const selected_text =
    STRING_ATTRIBUTE_TEXT("config:0-20");

int main(void) {
  return attributes[0]->mode != 0444 || attributes[0]->show != show_value ||
         show_value() != 17 || attributes[1] != 0 ||
         selected_text[0][0] != 'c' || selected_text[0][6] != ':';
}
