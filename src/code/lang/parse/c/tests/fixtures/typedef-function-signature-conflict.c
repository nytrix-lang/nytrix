/*
 * Negative fixture for conflicting function declarations through typedefs.
 */
unsigned int nyc_signature_conflict(unsigned int *value);

typedef unsigned int(nyc_signature_t)(unsigned int);
nyc_signature_t nyc_signature_conflict;
