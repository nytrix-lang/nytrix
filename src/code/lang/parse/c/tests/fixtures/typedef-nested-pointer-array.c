/*
 * C parser fixture for array typedefs containing nested pointer levels.
 */
typedef int *row[3];
extern row table[2];
row table[2] = {};

int main(void) { return table[0][0] != 0 || table[1][2] != 0; }
