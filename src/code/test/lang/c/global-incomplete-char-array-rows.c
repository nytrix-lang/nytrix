/*
 * C parser fixture for incomplete multidimensional character-array rows.
 */
static const char names[][4]
    __attribute__((nonstring, section(".init.rodata"))) = {
        "ab",
        [2] = "WXYZ",
};

int main(void) {
  return names[0][0] != 'a' || names[0][1] != 'b' || names[0][2] != '\0' ||
         names[1][0] != '\0' || names[2][0] != 'W' || names[2][3] != 'Z';
}
