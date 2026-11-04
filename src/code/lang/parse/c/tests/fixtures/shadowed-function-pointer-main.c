struct pmu { int type; };
struct hybrid { struct pmu pmu; };
struct ldt_struct { int entries; };
struct payload { unsigned long first; unsigned long second; };
struct mm_context { struct ldt_struct *ldt; };
struct mm_struct { struct mm_context context; };
struct task_struct { struct mm_struct *mm; };
int register_shadowed_pmu(struct hybrid *);
struct ldt_struct *typeof_member_pointer(void);
int register_pmu(struct pmu *pmu, const char *name, int type) {
  return pmu->type != 17 || name[0] != 't' || type != 0;
}
struct task_struct task;
struct mm_struct mm;
struct ldt_struct ldt;
struct task_struct *get_current(void) { return &task; }
extern struct payload assigned_payload;
void assign_global_payload(void);
const char *static_local_name(void);
int main(void) {
  struct hybrid hybrid = { .pmu = { .type = 17 } };
  task.mm = &mm;
  mm.context.ldt = &ldt;
  assign_global_payload();
  return register_shadowed_pmu(&hybrid) ||
         typeof_member_pointer() != &ldt ||
         assigned_payload.first != 19 || assigned_payload.second != 23 ||
         static_local_name()[0] != 's';
}
