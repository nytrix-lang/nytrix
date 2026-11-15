/*
 * Fixture: preserve loop control targets when unreachable code contains a
 * label nested inside a loop body.
 */
static int return_before_labeled_loop(void) {
  if (1)
    return -95;

  for (int scope = 0; scope || !1; ({ goto cleanup; }))
    if (0) {
    cleanup:
      break;
    } else {
      return -1;
    }

  return 0;
}

int main(void) { return return_before_labeled_loop() != -95; }
