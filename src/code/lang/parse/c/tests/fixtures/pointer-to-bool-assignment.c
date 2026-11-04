struct presence {
  _Bool set;
};

int main(void) {
  int value = 0;
  int *pointer = 0;
  _Bool present = 1;
  struct presence state = {pointer};
  if (_Generic(state.set, _Bool : 1, default : 0) != 1)
    return 5;
  if (_Generic((unsigned char)0, _Bool : 1, default : 0) != 0)
    return 6;
  present = pointer;
  if (present)
    return 1;
  if (state.set)
    return 2;
  state.set = pointer;
  if (state.set)
    return 3;
  pointer = &value;
  present = pointer;
  state.set = pointer;
  struct presence initialized = {pointer};
  return present && state.set && initialized.set ? 0 : 4;
}
