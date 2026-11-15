/*
 * Fixture: access promoted fields from a Plan 9 anonymous struct member.
 */
struct nyc_plan9_head {
  const char *name;
  int refcnt;
  void *aname;
};
struct nyc_plan9_file {
  struct nyc_plan9_head;
  const char iname[192 - sizeof(struct nyc_plan9_head)];
};
_Static_assert(__builtin_offsetof(struct nyc_plan9_file, iname) == 24,
               "Plan 9 anonymous aggregate member offset");
_Static_assert(sizeof(struct nyc_plan9_file) == 192,
               "Plan 9 anonymous aggregate member layout");
