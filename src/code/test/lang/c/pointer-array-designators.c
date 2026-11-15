/*
 * Fixture: initialize pointer arrays with repeated and indexed designators.
 */
enum { LABEL_ZERO = 0, LABEL_TWO = 2, LABEL_FOUR = 4 };
static const char *const labels[5] = {
    [LABEL_TWO] = "two",   "three",
    [LABEL_TWO] = "TWO",   [3] = 0,
    [LABEL_ZERO] = "zero", [LABEL_FOUR] = "four"};
static const char *const unset_labels[3];
int main(void) {
  return labels[0] == 0 || labels[0][0] != 'z' || labels[1] != 0 ||
         labels[2] == 0 || labels[2][0] != 'T' || labels[3] != 0 ||
         labels[4] == 0 || labels[4][0] != 'f' || unset_labels[0] != 0 ||
         unset_labels[2] != 0;
}
