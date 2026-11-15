/*
 * Parser fixture host: compare static and function-scope aggregate alignment.
 */
long file_scope_aggregate_alignment(void);
long function_scope_aggregate_array_alignment(void);

int main(void) {
  if (file_scope_aggregate_alignment() != 0)
    return 1;
  return function_scope_aggregate_array_alignment() != 0 ? 2 : 0;
}
