static unsigned int value;

int main(void) {
  *((unsigned int *)&(value)) = 7;
  return value != 7;
}
