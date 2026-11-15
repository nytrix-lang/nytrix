/*
 * C parser fixture for typeof on an address through a local aggregate pointer.
 */
struct nyc_typeof_pointer_member {
  void *private;
};

int main(void) {
  struct nyc_typeof_pointer_member holder;
  void *marker = &holder;
  holder.private = marker;
  typeof(*&holder.private) copy = holder.private;
  return copy != marker;
}
