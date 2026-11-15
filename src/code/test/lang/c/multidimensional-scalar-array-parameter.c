/*
 * Fixture: preserve row stride for a multidimensional array parameter.
 */
static int matrix[2][3] = {{1, 2, 3}, {4, 5, 6}};

static int select_element(const int values[2][3]) { return values[1][2]; }

int main(void) { return select_element(matrix) == 6 ? 0 : 1; }
