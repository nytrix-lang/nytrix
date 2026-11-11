/*
 * Host-side harness for calls using register-bound global operands.
 */
void nyc_register_constraint_callee(void) {}
int nyc_register_call_constraint(void);
int main(void) { return nyc_register_call_constraint() != 17; }
