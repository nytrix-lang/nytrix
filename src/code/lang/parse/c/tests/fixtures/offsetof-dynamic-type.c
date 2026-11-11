/*
 * C parser fixture for offsetof applied to a dynamically sized member type.
 */
struct dirent {
  unsigned long ino;
  char d_name[256];
};

int main(void) {
  struct dirent *dirent = 0;
  unsigned int index = 3;
  return sizeof(typeof(__builtin_offsetof(
             typeof(*dirent), d_name[index + 2]))) == sizeof(unsigned long)
             ? 0
             : 1;
}
