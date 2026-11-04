int main(void) {
  static const char names[5][3] = {
      [0 ... 4] = "??",
      [1] = "rd",
      [3] = "ex",
  };
  if (names[0][0] != '?' || names[0][1] != '?' || names[0][2] != '\0' ||
      names[1][0] != 'r' || names[1][1] != 'd' || names[1][2] != '\0' ||
      names[2][0] != '?' || names[2][1] != '?' || names[2][2] != '\0' ||
      names[3][0] != 'e' || names[3][1] != 'x' || names[3][2] != '\0' ||
      names[4][0] != '?' || names[4][1] != '?' || names[4][2] != '\0')
    return 1;
  return 0;
}
