struct OpaqueType;
int take_fwd(struct OpaqueType *ptr) {
  return ptr == 0 ? 0 : 1;
}

struct MyStruct;
struct MyStruct {
  long long x;
};
