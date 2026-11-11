/*
 * C parser fixture for incomplete multidimensional integer-array rows.
 */
static unsigned short translations[][3] = {
    [1] = {0x100, 0x101, 0x102},
    [3] = {0x200, 0x201, 0x202},
};

int main(void) {
  return translations[0][0] != 0 || translations[1][2] != 0x102 ||
         translations[2][1] != 0 || translations[3][0] != 0x200 ||
         translations[3][2] != 0x202;
}
