/*
 * Single-translation-unit parser amalgamation: includes all parse/
 * sources to produce one compilation unit for faster non-LTO builds.
 */
#include "core.c"
#include "expr.c"
#include "match.c"
#include "proof.c"
#include "stmt/init.c"
#include "stmtflow.c"
