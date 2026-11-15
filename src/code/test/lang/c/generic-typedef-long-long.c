/*
 * Fixture: keep typedef'd long and long long generic arms distinct.
 */
typedef unsigned long efi_status_t;
typedef unsigned long long u64;

int main(void) {
  return _Generic((efi_status_t)0, efi_status_t: 0, u64: 1, default: 2);
}
