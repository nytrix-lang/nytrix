/* AppKit NSTextInputClient bridge. Included by ffi.c so both the standalone
 * runtime and the JIT expose identical native ABI callbacks. */
#ifndef NY_RUNTIME_COCOA_INPUT_H
#define NY_RUNTIME_COCOA_INPUT_H

int64_t rt_cocoa_text_install(int64_t cls_value);
int64_t rt_cocoa_text_enable(int64_t view_value, int64_t x, int64_t y, int64_t w, int64_t h);
int64_t rt_cocoa_text_disable(int64_t view_value);
int64_t rt_cocoa_text_enabled(int64_t view_value);
int64_t rt_cocoa_text_marked(int64_t view_value);
int64_t rt_cocoa_text_take(int64_t view_value);
int64_t rt_cocoa_text_forget(int64_t view_value);

#if defined(__APPLE__) || defined(NY_COCOA_INPUT_TEST)
/* AppKit is loaded lazily. Do not make the generic runtime depend on it. */
typedef struct { uintptr_t location, length; } ny_cocoa_range;
typedef struct { double x, y, w, h; } ny_cocoa_rect;
typedef struct { double x, y; } ny_cocoa_point;
typedef void (*ny_cocoa_imp)(void);
static void *ny_cocoa_send;
static void *ny_cocoa_stret;
static void *(*ny_cocoa_sel)(const char *);
static void *(*ny_cocoa_class)(const char *);
static signed char (*ny_cocoa_add)(void *, void *, ny_cocoa_imp, const char *);
static void *(*ny_cocoa_protocol)(const char *);
static signed char (*ny_cocoa_conform)(void *, void *);

#define COCOA_MSG(ret, ...) ((ret (*)(void *, void *, ##__VA_ARGS__))ny_cocoa_send)
#define COCOA_SEL(name) ny_cocoa_sel(name)
#define COCOA_NOT_FOUND ((uintptr_t)INTPTR_MAX)

/* These structures contain only native-owned data. No Nytrix heap value is
 * retained across an AppKit callback or run-loop iteration. All access is on
 * AppKit's main thread. */
typedef struct ny_cocoa_input_event {
  struct ny_cocoa_input_event *next;
  char *text;
  int64_t cursor;
  int kind;
} ny_cocoa_input_event;
typedef struct ny_cocoa_input_view {
  struct ny_cocoa_input_view *next;
  void *view;
  bool enabled;
  char *marked;
  uintptr_t marked_length;
  ny_cocoa_range selected;
  ny_cocoa_rect caret;
  ny_cocoa_input_event *head, *tail;
} ny_cocoa_input_view;
static ny_cocoa_input_view *ny_cocoa_inputs;
extern int64_t rt_append(int64_t lst, int64_t val);

static ny_cocoa_input_view *ny_cocoa_input_find(void *view, bool create) {
  for (ny_cocoa_input_view *s = ny_cocoa_inputs; s; s = s->next)
    if (s->view == view) return s;
  if (!view || !create) return NULL;
  ny_cocoa_input_view *s = calloc(1, sizeof(*s));
  if (!s) return NULL;
  s->view = view;
  s->caret = (ny_cocoa_rect){0, 0, 1, 1};
  s->next = ny_cocoa_inputs;
  ny_cocoa_inputs = s;
  return s;
}

static void ny_cocoa_input_queue(ny_cocoa_input_view *s, int kind,
                                 const char *text, int64_t cursor) {
  ny_cocoa_input_event *event = calloc(1, sizeof(*event));
  if (!event) return;
  event->text = strdup(text ? text : "");
  if (!event->text) { free(event); return; }
  event->kind = kind;
  event->cursor = cursor;
  if (s->tail) s->tail->next = event;
  else s->head = event;
  s->tail = event;
}

/* UTF-16 NSString offsets become UTF-8 byte offsets, as in the Wayland and
 * Windows preedit events. Never split a surrogate pair or UTF-8 sequence. */
static int64_t ny_cocoa_input_cursor(const char *text, uintptr_t units) {
  const unsigned char *p = (const unsigned char *)text;
  uintptr_t count = 0;
  size_t off = 0;
  if (units == COCOA_NOT_FOUND) return -1;
  while (p[off] && count < units) {
    unsigned char c = p[off];
    size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    uintptr_t width = n == 4 ? 2 : 1;
    if (count + width > units) break;
    size_t i = 1;
    while (i < n && p[off + i] && (p[off + i] & 0xc0) == 0x80) ++i;
    if (i != n) break;
    off += n;
    count += width;
  }
  return (int64_t)off;
}

static void *ny_cocoa_input_string(void *value) {
  if (!value) return NULL;
  if (COCOA_MSG(signed char, void *)(value, COCOA_SEL("isKindOfClass:"),
                                      ny_cocoa_class("NSAttributedString")))
    return COCOA_MSG(void *)(value, COCOA_SEL("string"));
  return value;
}

static void ny_cocoa_input_clear(ny_cocoa_input_view *s) {
  if (!s || !s->marked) return;
  free(s->marked);
  s->marked = NULL;
  s->marked_length = 0;
  s->selected = (ny_cocoa_range){0, 0};
  ny_cocoa_input_queue(s, 0, "", -1);
}

static signed char ny_cocoa_accepts(void *self, void *cmd) {
  (void)self; (void)cmd;
  return 1;
}
static signed char ny_cocoa_has_marked(void *self, void *cmd) {
  (void)cmd;
  ny_cocoa_input_view *s = ny_cocoa_input_find(self, false);
  return s && s->marked_length != 0;
}
static ny_cocoa_range ny_cocoa_marked_range(void *self, void *cmd) {
  (void)cmd;
  ny_cocoa_input_view *s = ny_cocoa_input_find(self, false);
  return s && s->marked_length ? (ny_cocoa_range){0, s->marked_length}
                               : (ny_cocoa_range){COCOA_NOT_FOUND, 0};
}
static ny_cocoa_range ny_cocoa_selected_range(void *self, void *cmd) {
  (void)cmd;
  ny_cocoa_input_view *s = ny_cocoa_input_find(self, false);
  return s ? s->selected : (ny_cocoa_range){0, 0};
}
static void ny_cocoa_mark(void *self, void *cmd, void *value,
                           ny_cocoa_range selected, ny_cocoa_range replacement) {
  (void)cmd; (void)replacement;
  ny_cocoa_input_view *s = ny_cocoa_input_find(self, false);
  if (!s || !s->enabled) return;
  void *str = ny_cocoa_input_string(value);
  const char *text = str ? COCOA_MSG(const char *)(str, COCOA_SEL("UTF8String")) : NULL;
  if (!text || !*text) { ny_cocoa_input_clear(s); return; }
  char *copy = strdup(text);
  if (!copy) return;
  free(s->marked);
  s->marked = copy;
  s->marked_length = COCOA_MSG(uintptr_t)(str, COCOA_SEL("length"));
  if (selected.location != COCOA_NOT_FOUND && selected.location > s->marked_length)
    selected.location = s->marked_length;
  if (selected.location == COCOA_NOT_FOUND) selected.length = 0;
  else if (selected.length > s->marked_length - selected.location)
    selected.length = s->marked_length - selected.location;
  s->selected = selected;
  ny_cocoa_input_queue(s, 0, copy, ny_cocoa_input_cursor(copy, selected.location));
}
static void ny_cocoa_unmark(void *self, void *cmd) {
  (void)cmd;
  ny_cocoa_input_clear(ny_cocoa_input_find(self, false));
}
static void ny_cocoa_insert(void *self, void *cmd, void *value,
                             ny_cocoa_range replacement) {
  (void)cmd; (void)replacement;
  ny_cocoa_input_view *s = ny_cocoa_input_find(self, false);
  if (!s || !s->enabled) return;
  void *str = ny_cocoa_input_string(value);
  const char *text = str ? COCOA_MSG(const char *)(str, COCOA_SEL("UTF8String")) : NULL;
  ny_cocoa_input_clear(s);
  if (text && *text) ny_cocoa_input_queue(s, 1, text, -1);
}
static void *ny_cocoa_attributes(void *self, void *cmd) {
  (void)self; (void)cmd;
  return COCOA_MSG(void *)(ny_cocoa_class("NSArray"), COCOA_SEL("array"));
}
static void *ny_cocoa_substring(void *self, void *cmd, ny_cocoa_range requested,
                                ny_cocoa_range *actual) {
  (void)self; (void)cmd; (void)requested;
  /* The core event API has no document snapshot. Never claim surrounding text. */
  if (actual) *actual = (ny_cocoa_range){COCOA_NOT_FOUND, 0};
  return NULL;
}
static uintptr_t ny_cocoa_character_index(void *self, void *cmd, ny_cocoa_point p) {
  (void)self; (void)cmd; (void)p;
  return COCOA_NOT_FOUND;
}
static ny_cocoa_rect ny_cocoa_first_rect(void *self, void *cmd,
                                       ny_cocoa_range requested, ny_cocoa_range *actual) {
  (void)cmd;
  ny_cocoa_input_view *s = ny_cocoa_input_find(self, false);
  if (actual) *actual = requested;
  if (!s) return (ny_cocoa_rect){0, 0, 0, 0};
  ny_cocoa_rect bounds, local = s->caret, base, screen;
  void *window = COCOA_MSG(void *)(self, COCOA_SEL("window"));
  if (!window) return local;
#if defined(__x86_64__)
  ((void (*)(ny_cocoa_rect *, void *, void *))ny_cocoa_stret)(&bounds, self, COCOA_SEL("bounds"));
#else
  bounds = COCOA_MSG(ny_cocoa_rect)(self, COCOA_SEL("bounds"));
#endif
  if (!COCOA_MSG(signed char)(self, COCOA_SEL("isFlipped")))
    local.y = bounds.h - local.y - local.h;
#if defined(__x86_64__)
  ((void (*)(ny_cocoa_rect *, void *, void *, ny_cocoa_rect, void *))ny_cocoa_stret)
      (&base, self, COCOA_SEL("convertRect:toView:"), local, NULL);
  ((void (*)(ny_cocoa_rect *, void *, void *, ny_cocoa_rect))ny_cocoa_stret)
      (&screen, window, COCOA_SEL("convertRectToScreen:"), base);
#else
  base = COCOA_MSG(ny_cocoa_rect, ny_cocoa_rect, void *)(self, COCOA_SEL("convertRect:toView:"), local, NULL);
  screen = COCOA_MSG(ny_cocoa_rect, ny_cocoa_rect)(window, COCOA_SEL("convertRectToScreen:"), base);
#endif
  return screen;
}
static void ny_cocoa_command(void *self, void *cmd, void *selector) {
  (void)self; (void)cmd; (void)selector;
  /* Key commands are already represented by Nytrix key events. */
}
static void ny_cocoa_key_down(void *self, void *cmd, void *event) {
  (void)cmd;
  ny_cocoa_input_view *s = ny_cocoa_input_find(self, false);
  if (!s || !s->enabled) return;
  uintptr_t mods = COCOA_MSG(uintptr_t)(event, COCOA_SEL("modifierFlags"));
  if (mods & ((1u << 18) | (1u << 20))) return;
  void *events = COCOA_MSG(void *, void *)(ny_cocoa_class("NSArray"), COCOA_SEL("arrayWithObject:"), event);
  COCOA_MSG(void, void *)(self, COCOA_SEL("interpretKeyEvents:"), events);
}

int64_t rt_cocoa_text_install(int64_t cls_value) {
  void *cls = (void *)(uintptr_t)rt_ffi_untag_ptr(cls_value);
  if (!cls) return NY_IMM_FALSE;
  ny_cocoa_send = dlsym(RTLD_DEFAULT, "objc_msgSend");
  ny_cocoa_stret = dlsym(RTLD_DEFAULT, "objc_msgSend_stret");
  ny_cocoa_sel = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
  ny_cocoa_class = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
  ny_cocoa_add = (signed char (*)(void *, void *, ny_cocoa_imp, const char *))dlsym(RTLD_DEFAULT, "class_addMethod");
  ny_cocoa_protocol = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "objc_getProtocol");
  ny_cocoa_conform = (signed char (*)(void *, void *))dlsym(RTLD_DEFAULT, "class_addProtocol");
  if (!ny_cocoa_send || !ny_cocoa_sel || !ny_cocoa_class || !ny_cocoa_add ||
      !ny_cocoa_protocol || !ny_cocoa_conform) return NY_IMM_FALSE;
#if defined(__x86_64__)
  if (!ny_cocoa_stret) return NY_IMM_FALSE;
#endif
  void *protocol = ny_cocoa_protocol("NSTextInputClient");
  if (!protocol) return NY_IMM_FALSE;
  bool ok = true;
#define ADD(name, func, types) do { if (!ny_cocoa_add(cls, COCOA_SEL(name), (ny_cocoa_imp)func, types)) ok = false; } while (0)
  ADD("acceptsFirstResponder", ny_cocoa_accepts, "c@:");
  ADD("hasMarkedText", ny_cocoa_has_marked, "c@:");
  ADD("markedRange", ny_cocoa_marked_range, "{_NSRange=QQ}@:");
  ADD("selectedRange", ny_cocoa_selected_range, "{_NSRange=QQ}@:");
  ADD("setMarkedText:selectedRange:replacementRange:", ny_cocoa_mark, "v@:@{_NSRange=QQ}{_NSRange=QQ}");
  ADD("unmarkText", ny_cocoa_unmark, "v@:");
  ADD("insertText:replacementRange:", ny_cocoa_insert, "v@:@{_NSRange=QQ}");
  ADD("validAttributesForMarkedText", ny_cocoa_attributes, "@@:");
  ADD("attributedSubstringForProposedRange:actualRange:", ny_cocoa_substring, "@@:{_NSRange=QQ}^{_NSRange=QQ}");
  ADD("characterIndexForPoint:", ny_cocoa_character_index, "Q@:{CGPoint=dd}");
  ADD("firstRectForCharacterRange:actualRange:", ny_cocoa_first_rect, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{_NSRange=QQ}^{_NSRange=QQ}");
  ADD("doCommandBySelector:", ny_cocoa_command, "v@::");
  ADD("keyDown:", ny_cocoa_key_down, "v@:@");
#undef ADD
  return ok && ny_cocoa_conform(cls, protocol) ? NY_IMM_TRUE : NY_IMM_FALSE;
}

int64_t rt_cocoa_text_enable(int64_t view_value, int64_t x, int64_t y,
                              int64_t w, int64_t h) {
  void *view = (void *)(uintptr_t)rt_ffi_untag_ptr(view_value);
  if (!ny_cocoa_send || !view) return NY_IMM_FALSE;
  ny_cocoa_input_view *s = ny_cocoa_input_find(view, true);
  if (!s) return NY_IMM_FALSE;
  s->caret = (ny_cocoa_rect){rt_untag_v(x), rt_untag_v(y), rt_untag_v(w), rt_untag_v(h)};
  void *window = COCOA_MSG(void *)(view, COCOA_SEL("window"));
  if (!window || !COCOA_MSG(signed char, void *)(window, COCOA_SEL("makeFirstResponder:"), view))
    return NY_IMM_FALSE;
  s->enabled = true;
  return NY_IMM_TRUE;
}
int64_t rt_cocoa_text_disable(int64_t view_value) {
  void *view = (void *)(uintptr_t)rt_ffi_untag_ptr(view_value);
  ny_cocoa_input_view *s = ny_cocoa_input_find(view, false);
  if (!s) return NY_IMM_TRUE;
  if (s->enabled) {
    s->enabled = false;
    void *context = COCOA_MSG(void *)(view, COCOA_SEL("inputContext"));
    if (context) COCOA_MSG(void)(context, COCOA_SEL("discardMarkedText"));
    ny_cocoa_input_clear(s);
  }
  return NY_IMM_TRUE;
}
int64_t rt_cocoa_text_enabled(int64_t view_value) {
  ny_cocoa_input_view *s = ny_cocoa_input_find((void *)(uintptr_t)rt_ffi_untag_ptr(view_value), false);
  return s && s->enabled ? NY_IMM_TRUE : NY_IMM_FALSE;
}
int64_t rt_cocoa_text_marked(int64_t view_value) {
  ny_cocoa_input_view *s = ny_cocoa_input_find((void *)(uintptr_t)rt_ffi_untag_ptr(view_value), false);
  return rt_alloc_string(s && s->marked ? s->marked : "");
}
int64_t rt_cocoa_text_take(int64_t view_value) {
  ny_cocoa_input_view *s = ny_cocoa_input_find((void *)(uintptr_t)rt_ffi_untag_ptr(view_value), false);
  if (!s || !s->head) return NY_IMM_NIL;
  ny_cocoa_input_event *e = s->head;
  s->head = e->next;
  if (!s->head) s->tail = NULL;
  int64_t result = rt_list_new_raw(3);
  result = rt_append(result, rt_tag_v(e->kind));
  result = rt_append(result, rt_alloc_string(e->text));
  result = rt_append(result, rt_tag_v(e->cursor));
  free(e->text);
  free(e);
  return result;
}
int64_t rt_cocoa_text_forget(int64_t view_value) {
  void *view = (void *)(uintptr_t)rt_ffi_untag_ptr(view_value);
  for (ny_cocoa_input_view **link = &ny_cocoa_inputs; *link; link = &(*link)->next) {
    ny_cocoa_input_view *s = *link;
    if (s->view != view) continue;
    *link = s->next;
    free(s->marked);
    while (s->head) {
      ny_cocoa_input_event *e = s->head;
      s->head = e->next;
      free(e->text);
      free(e);
    }
    free(s);
    break;
  }
  return NY_IMM_TRUE;
}
#undef COCOA_MSG
#undef COCOA_SEL
#undef COCOA_NOT_FOUND
#else
int64_t rt_cocoa_text_install(int64_t cls) { (void)cls; return NY_IMM_FALSE; }
int64_t rt_cocoa_text_enable(int64_t view, int64_t x, int64_t y, int64_t w, int64_t h) {
  (void)view; (void)x; (void)y; (void)w; (void)h; return NY_IMM_FALSE;
}
int64_t rt_cocoa_text_disable(int64_t view) { (void)view; return NY_IMM_FALSE; }
int64_t rt_cocoa_text_enabled(int64_t view) { (void)view; return NY_IMM_FALSE; }
int64_t rt_cocoa_text_marked(int64_t view) { (void)view; return rt_alloc_string(""); }
int64_t rt_cocoa_text_take(int64_t view) { (void)view; return NY_IMM_NIL; }
int64_t rt_cocoa_text_forget(int64_t view) { (void)view; return NY_IMM_FALSE; }
#endif
#endif
