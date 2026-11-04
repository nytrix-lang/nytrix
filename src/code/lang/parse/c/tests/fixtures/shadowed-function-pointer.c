struct pmu { int type; };
struct hybrid { struct pmu pmu; };
struct pmu *hybrid_pmu(struct pmu *);
int register_pmu(struct pmu *, const char *, int);
int register_shadowed_pmu(struct hybrid *hybrid_pmu) {
  return register_pmu(&hybrid_pmu->pmu, "test", 0);
}
struct ldt_struct { int entries; };
struct label { const char *name; };
struct payload { unsigned long first; unsigned long second; };
struct mm_context { struct ldt_struct *ldt; };
struct mm_struct { struct mm_context context; };
struct task_struct { struct mm_struct *mm; };
struct task_struct *get_current(void);
struct payload assigned_payload;
const char *static_local_name(void) {
  static struct label value = { .name = ("static-name") };
  return value.name;
}
void assign_global_payload(void) {
  assigned_payload = (struct payload){ .first = 19, .second = 23 };
}
struct ldt_struct *typeof_member_pointer(void) {
  typeof(*&get_current()->mm->context.ldt) value =
      *&get_current()->mm->context.ldt;
  return value;
}
