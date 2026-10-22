#include "code/lang/types/test.h"
#include "code/lang/parse/proof.h"
#include "code/lang/types/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool contains(const char *const *labels, size_t count,
                     const char *label) {
  for (size_t i = 0; i < count; ++i)
    if (strcmp(labels[i], label) == 0)
      return true;
  return false;
}

static size_t row_label_count(ny_type_t *row, const char *label) {
  size_t count = 0;
  for (ny_type_t *curr = ny_type_find(row);
       curr && curr->kind == NY_TYPE_ROW && curr->as.row.label;
       curr = ny_type_find(curr->as.row.tail)) {
    if (strcmp(curr->as.row.label, label) == 0)
      count++;
  }
  return count;
}

/*
 * Deliberately independent of NY_TYPE_ROW traversal and unification.
 */
static bool oracle_equal(const char *const *a, size_t a_count,
                         const char *const *b, size_t b_count) {
  for (size_t i = 0; i < a_count; ++i)
    if (!contains(b, b_count, a[i]))
      return false;
  for (size_t i = 0; i < b_count; ++i)
    if (!contains(a, a_count, b[i]))
      return false;
  return true;
}

static ny_type_t *make_row(ny_type_arena_t *arena, const char *const *labels,
                           size_t count) {
  ny_type_t *row = NULL;
  ny_type_t *field = ny_type_concrete(arena, "effect");
  for (size_t i = count; i > 0; --i)
    row = ny_type_row(arena, labels[i - 1], field, row);
  return row;
}

static bool check_closed_sets(void) {
  static const char *const universe[] = {"alpha", "beta", "gamma"};
  static const size_t permutations[][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2},
                                           {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
  for (unsigned left = 0; left < 8; ++left) {
    for (size_t left_order = 0; left_order < 6; ++left_order) {
      const char *left_labels[3];
      size_t left_count = 0;
      for (unsigned i = 0; i < 3; ++i) {
        unsigned bit = permutations[left_order][i];
        if (left & (1u << bit))
          left_labels[left_count++] = universe[bit];
      }
      for (unsigned right = 0; right < 8; ++right) {
        for (size_t right_order = 0; right_order < 6; ++right_order) {
          const char *right_labels[3];
          size_t right_count = 0;
          for (unsigned i = 0; i < 3; ++i) {
            unsigned bit = permutations[right_order][i];
            if (right & (1u << bit))
              right_labels[right_count++] = universe[bit];
          }
          ny_type_arena_t arena;
          ny_type_arena_init(&arena);
          ny_type_t *a = make_row(&arena, left_labels, left_count);
          ny_type_t *b = make_row(&arena, right_labels, right_count);
          bool expected =
              oracle_equal(left_labels, left_count, right_labels, right_count);
          bool actual = ny_type_unify(a, b);
          ny_type_arena_reset(&arena);
          if (actual != expected) {
            fprintf(stderr,
                    "[row-algebra] closed set mismatch left=%u order=%zu "
                    "right=%u order=%zu expected=%d actual=%d\n",
                    left, left_order, right, right_order, expected, actual);
            return false;
          }
        }
      }
    }
  }
  return true;
}

static bool check_duplicate_elimination(void) {
  ny_type_arena_t arena;
  ny_type_arena_init(&arena);
  ny_type_t *field = ny_type_concrete(&arena, "effect");
  ny_type_t *duplicate = ny_type_row(&arena, "alpha", field,
                                     ny_type_row(&arena, "alpha", field, NULL));
  ny_type_t *single = ny_type_row(&arena, "alpha", field, NULL);
  bool ok = ny_type_unify(duplicate, single);
  ny_type_arena_reset(&arena);
  if (!ok)
    fprintf(stderr, "[row-algebra] duplicate labels did not normalize\n");
  if (!ok)
    return false;

  ny_type_arena_init(&arena);
  ny_type_t *first = ny_type_concrete(&arena, "first");
  ny_type_t *second = ny_type_concrete(&arena, "second");
  duplicate = ny_type_row(&arena, "alpha", first,
                          ny_type_row(&arena, "alpha", second, NULL));
  single = ny_type_row(&arena, "alpha", first, NULL);
  bool rejected_conflict = !ny_type_unify(duplicate, single);
  ny_type_arena_reset(&arena);
  if (!rejected_conflict)
    fprintf(stderr,
            "[row-algebra] conflicting duplicate fields were accepted\n");
  return rejected_conflict;
}

static bool check_open_row_unification(void) {
  ny_type_arena_t arena;
  ny_type_arena_init(&arena);
  ny_type_t *field = ny_type_concrete(&arena, "effect");
  ny_type_t *left_tail = ny_type_effect_var(&arena);
  ny_type_t *right_tail = ny_type_effect_var(&arena);
  ny_type_t *left = ny_type_row(&arena, "alpha", field, left_tail);
  ny_type_t *right = ny_type_row(
      &arena, "beta", field, ny_type_row(&arena, "alpha", field, right_tail));
  bool unified = ny_type_unify(left, right);
  ny_type_t *left_residual = ny_type_find(left_tail);
  ny_type_t *right_residual = ny_type_find(right_tail);
  bool residuals_match =
      unified && ny_type_row_has_label(left_residual, "beta") &&
      ny_type_row_has_label(right, "beta") && right_residual == right_tail;
  ny_type_arena_reset(&arena);
  if (!residuals_match)
    fprintf(stderr, "[row-algebra] open-row residuals were not preserved\n");
  return residuals_match;
}

static bool check_occurs_rejection(void) {
  ny_type_arena_t arena;
  ny_type_arena_init(&arena);
  ny_type_t *tail = ny_type_effect_var(&arena);
  ny_type_t *infinite =
      ny_type_row(&arena, "alpha", ny_type_concrete(&arena, "effect"), tail);
  bool rejected = !ny_type_unify(tail, infinite);
  ny_type_arena_reset(&arena);
  if (!rejected)
    fprintf(stderr, "[row-algebra] infinite row escaped the occurs check\n");
  return rejected;
}

static bool check_closed_tail_stays_closed(void) {
  ny_type_arena_t arena;
  ny_type_arena_init(&arena);
  ny_type_t *field = ny_type_concrete(&arena, "effect");
  ny_type_t *tail = ny_type_effect_var(&arena);
  ny_type_t *open = ny_type_row(&arena, "alpha", field, tail);
  ny_type_t *closed = ny_type_row(&arena, "alpha", field, NULL);
  bool closed_once = ny_type_unify(open, closed);
  bool rejects_new_effect =
      !ny_type_unify(tail, ny_type_row(&arena, "beta", field, NULL));
  ny_type_arena_reset(&arena);
  if (!closed_once || !rejects_new_effect)
    fprintf(stderr, "[row-algebra] a closed callback row reopened\n");
  return closed_once && rejects_new_effect;
}

static bool check_distinct_open_tails(void) {
  ny_type_arena_t arena;
  ny_type_arena_init(&arena);
  ny_type_t *field = ny_type_concrete(&arena, "effect");
  ny_type_t *left_tail = ny_type_effect_var(&arena);
  ny_type_t *right_tail = ny_type_effect_var(&arena);
  ny_type_t *left = ny_type_row(&arena, "alpha", field, left_tail);
  ny_type_t *right = ny_type_row(&arena, "beta", field, right_tail);
  bool unified = ny_type_unify(left, right);
  bool left_accepts_residual =
      unified &&
      ny_type_unify(left_tail, ny_type_row(&arena, "beta", field, NULL));
  bool right_accepts_residual =
      left_accepts_residual &&
      ny_type_unify(right_tail, ny_type_row(&arena, "alpha", field, NULL));
  bool retains_effects = right_accepts_residual &&
                         ny_type_row_has_label(left, "alpha") &&
                         ny_type_row_has_label(left, "beta") &&
                         ny_type_row_has_label(right, "alpha") &&
                         ny_type_row_has_label(right, "beta");
  ny_type_arena_reset(&arena);
  if (!retains_effects)
    fprintf(stderr, "[row-algebra] distinct open tails lost their residuals\n");
  return retains_effects;
}

static bool check_row_compose_unifies_payloads(void) {
  ny_type_arena_t arena;
  ny_type_arena_init(&arena);
  ny_subst_t *subst = ny_subst_new(4);
  ny_type_t *payload = ny_type_var(&arena);
  ny_type_t *unit = ny_type_concrete(&arena, "unit");
  ny_type_t *left = ny_type_row(&arena, "library.effect", payload, NULL);
  ny_type_t *right = ny_type_row(&arena, "library.effect", unit, NULL);
  ny_type_t *composed =
      subst ? ny_type_row_compose(&arena, subst, left, right) : NULL;
  bool unified = composed && ny_type_find(payload) == unit &&
                 row_label_count(composed, "library.effect") == 1;
  ny_subst_free(subst);
  ny_type_arena_reset(&arena);

  ny_type_arena_init(&arena);
  left = ny_type_row(&arena, "library.effect", ny_type_concrete(&arena, "read"),
                     NULL);
  right = ny_type_row(&arena, "library.effect",
                      ny_type_concrete(&arena, "write"), NULL);
  bool rejects_conflict =
      ny_type_row_compose(&arena, NULL, left, right) == NULL;
  ny_type_arena_reset(&arena);
  if (!unified || !rejects_conflict)
    fprintf(stderr,
            "[row-algebra] composition did not unify matching effect payloads "
            "or reject conflicting ones\n");
  return unified && rejects_conflict;
}

static bool check_builtin_effect_masks_are_exact(void) {
  ny_type_arena_t arena;
  ny_type_arena_init(&arena);
  ny_type_t *pure = ny_type_effect(&arena, NY_FX_NONE);
  ny_type_t *all = ny_type_effect(&arena, NY_FX_ALL);
  ny_type_t *io = ny_type_effect(&arena, NY_FX_IO);
  ny_type_t *io_alloc = ny_type_effect(&arena, NY_FX_IO | NY_FX_ALLOC);
  bool exact = ny_type_unify(pure, pure) && ny_type_unify(all, all) &&
               ny_type_unify(io, io) && !ny_type_unify(pure, all) &&
               !ny_type_unify(io, all) && !ny_type_unify(io, io_alloc) &&
               !ny_type_row_has_label(pure, "std.effects.io") &&
               ny_type_row_has_label(io, "std.effects.io") &&
               ny_type_row_has_label(all, "std.effects.read_memory") &&
               ny_type_row_has_label(all, "std.effects.write_memory");
  ny_type_arena_reset(&arena);
  if (!exact)
    fprintf(stderr,
            "[row-algebra] builtin effect identities were not exact rows\n");
  return exact;
}

static bool check_capability_effect_identity(void) {
  ny_type_arena_t arena;
  ny_type_arena_init(&arena);
  ny_type_t *alpha = ny_type_capability(&arena, "alpha");
  ny_type_t *alpha_again = ny_type_capability(&arena, "alpha");
  ny_type_t *case_distinct = ny_type_capability(&arena, "Alpha");
  ny_type_t *beta = ny_type_capability(&arena, "beta");
  ny_type_t *plain = ny_type_effect(&arena, NY_FX_NONE);
  char max_name[NY_TYPE_CAPABILITY_NAME_MAX + 1];
  char oversized_name[NY_TYPE_CAPABILITY_NAME_MAX + 2];
  memset(max_name, 'a', sizeof(max_name) - 1);
  max_name[sizeof(max_name) - 1] = '\0';
  memset(oversized_name, 'a', sizeof(oversized_name) - 1);
  oversized_name[sizeof(oversized_name) - 1] = '\0';
  bool exact =
      ny_type_unify(alpha, alpha_again) && !ny_type_unify(alpha, beta) &&
      !ny_type_unify(alpha, case_distinct) && !ny_type_unify(alpha, plain) &&
      ny_type_capability_effect(&arena, max_name, NULL) != NULL &&
      ny_type_capability_effect(&arena, oversized_name, NULL) == NULL &&
      alpha->kind == NY_TYPE_CAPABILITY && plain->kind == NY_TYPE_ROW;
  ny_type_arena_reset(&arena);
  if (!exact)
    fprintf(stderr,
            "[row-algebra] capability identity was not a distinct type\n");
  return exact;
}

static bool check_capability_rows_are_distinct_and_serializable(void) {
  ny_type_arena_t arena;
  ny_type_arena_init(&arena);
  ny_type_t *read_write = ny_type_capability_effect(
      &arena, "read", ny_type_capability_effect(&arena, "write", NULL));
  ny_type_t *write_read = ny_type_capability_effect(
      &arena, "write", ny_type_capability_effect(&arena, "read", NULL));
  ny_type_t *read_identity = ny_type_capability_effect(&arena, "read", NULL);
  ny_type_t *module_owned_identity =
      ny_type_capability_effect(&arena, "project.math.read", NULL);
  ny_type_t *wrong_read_identity = ny_type_row(
      &arena, "ny.capability.read", ny_type_capability(&arena, "write"), NULL);
  char *serialized = ny_type_effect_row_serialize(read_write);
  char *module_owned_text = ny_type_effect_row_serialize(module_owned_identity);
  ny_type_t *separator_capability =
      ny_type_capability_effect(&arena, "read|write", NULL);
  char *separator_text = ny_type_effect_row_serialize(separator_capability);
  ny_type_arena_t decoded_arena;
  ny_type_arena_init(&decoded_arena);
  ny_type_t *decoded =
      serialized ? ny_type_effect_row_deserialize(&decoded_arena, serialized)
                 : NULL;
  char *decoded_text = decoded ? ny_type_effect_row_serialize(decoded) : NULL;
  ny_type_t *decoded_module_owned =
      module_owned_text
          ? ny_type_effect_row_deserialize(&decoded_arena, module_owned_text)
          : NULL;
  ny_type_t *decoded_separator =
      separator_text
          ? ny_type_effect_row_deserialize(&decoded_arena, separator_text)
          : NULL;
  char *decoded_separator_text =
      decoded_separator ? ny_type_effect_row_serialize(decoded_separator)
                        : NULL;
  bool imported_identity_is_scoped =
      decoded_module_owned && read_identity &&
      !ny_type_unify(decoded_module_owned, read_identity);
  bool valid = read_write && write_read &&
               ny_type_row_has_label(read_write, "ny.capability.read") &&
               ny_type_row_has_label(read_write, "ny.capability.write") &&
               ny_type_unify(read_write, write_read) && serialized &&
               decoded_text && strcmp(serialized, decoded_text) == 0 &&
               module_owned_text && decoded_module_owned &&
               ny_type_row_has_label(decoded_module_owned,
                                     "ny.capability.project.math.read") &&
               imported_identity_is_scoped && separator_text &&
               decoded_separator_text &&
               strcmp(separator_text, decoded_separator_text) == 0 &&
               wrong_read_identity &&
               !ny_type_unify(read_identity, wrong_read_identity) &&
               strstr(serialized, "ny.capability.read=cap%28read%29") &&
               strstr(serialized, "ny.capability.write=cap%28write%29");
  free(decoded_text);
  free(module_owned_text);
  free(decoded_separator_text);
  ny_type_arena_reset(&decoded_arena);
  free(serialized);
  free(separator_text);
  ny_type_arena_reset(&arena);
  if (!valid)
    fprintf(
        stderr,
        "[row-algebra] multiple capability effects were not distinct rows\n");
  return valid;
}

static bool check_module_owned_capability_identity(void) {
  char qualified[NY_TYPE_CAPABILITY_NAME_MAX + 1] = {0};
  char explicit_name[NY_TYPE_CAPABILITY_NAME_MAX + 1] = {0};
  char top_level[NY_TYPE_CAPABILITY_NAME_MAX + 1] = {0};
  char boundary_capability[62];
  char oversized_capability[63];
  char long_module[61];
  memset(boundary_capability, 'c', sizeof(boundary_capability) - 1);
  boundary_capability[sizeof(boundary_capability) - 1] = '\0';
  memset(oversized_capability, 'c', sizeof(oversized_capability) - 1);
  oversized_capability[sizeof(oversized_capability) - 1] = '\0';
  memset(long_module, 'm', sizeof(long_module) - 1);
  long_module[sizeof(long_module) - 1] = '\0';
  bool valid =
      ny_type_capability_identity("library.storage", "read", qualified) &&
      strcmp(qualified, "library.storage.read") == 0 &&
      ny_type_capability_identity("library.storage", "shared.read",
                                  explicit_name) &&
      strcmp(explicit_name, "shared.read") == 0 &&
      ny_type_capability_identity(NULL, "read", top_level) &&
      strcmp(top_level, "read") == 0 &&
      ny_type_capability_identity("m", boundary_capability, qualified) &&
      strlen(qualified) == NY_TYPE_CAPABILITY_NAME_MAX &&
      !ny_type_capability_identity("m", oversized_capability, qualified) &&
      !ny_type_capability_identity(long_module, "read", qualified) &&
      !ny_type_capability_identity("library.storage", "", qualified);
  if (!valid)
    fprintf(stderr, "[row-algebra] module-owned capability identity resolution "
                    "failed\n");
  return valid;
}

static bool check_external_effect_variables_are_isolated(void) {
  ny_type_arena_t arena;
  ny_type_arena_init(&arena);
  ny_subst_t *subst = ny_subst_new(8);
  ny_type_t *left = ny_type_effect_var(NULL);
  ny_type_t *right = ny_type_effect_var(NULL);
  ny_type_t *io = ny_type_effect(&arena, NY_FX_IO);
  bool unified =
      subst && left && right && io && ny_type_unify_with_subst(subst, left, io);
  if (unified)
    ny_subst_apply(subst, right);
  bool isolated = unified && ny_type_find(left) == io &&
                  ny_type_find(right) == right &&
                  ny_type_find(right)->kind == NY_TYPE_EFFECT_VAR;
  ny_subst_free(subst);
  ny_type_arena_reset(&arena);
  if (!isolated)
    fprintf(stderr,
            "[row-algebra] external effect variables shared a substitution\n");
  return isolated;
}

static bool check_effect_row_serialization_is_stable(void) {
  ny_type_arena_t arena;
  ny_type_arena_init(&arena);
  ny_type_t *unit = ny_type_concrete(&arena, "unit");
  ny_type_t *left =
      ny_type_row(&arena, "std.effects.io", unit,
                  ny_type_row(&arena, "std.effects.alloc", unit, NULL));
  ny_type_t *right =
      ny_type_row(&arena, "std.effects.alloc", unit,
                  ny_type_row(&arena, "std.effects.io", unit, NULL));
  ny_type_t *tail_a = ny_type_effect_var(&arena);
  ny_type_t *tail_b = ny_type_effect_var(&arena);
  ny_type_t *open_a = ny_type_row(&arena, "std.effects.io", unit, tail_a);
  ny_type_t *open_b = ny_type_row(&arena, "std.effects.io", unit, tail_b);
  ny_type_t *field_var = ny_type_var(&arena);
  ny_type_t *unstable_field =
      ny_type_row(&arena, "effect.with.payload", field_var, NULL);
  ny_type_t *string_type = ny_type_concrete(&arena, "string");
  ny_type_t *int_type = ny_type_concrete(&arena, "int");
  ny_type_t *list_type = ny_type_apply(&arena, "list", int_type, NULL, 1);
  ny_type_t *payload_state =
      ny_type_row(&arena, "payload.state", string_type, NULL);
  ny_type_t *arrow_effects = ny_type_row(&arena, "std.effects.io", unit, NULL);
  ny_type_t *payload_callback =
      ny_type_arrow_effects(&arena, list_type, payload_state, arrow_effects);
  ny_type_t *payload_type =
      ny_type_apply(&arena, "dict", string_type, payload_callback, 2);
  ny_type_t *numeric_index =
      ny_type_apply_bounded(&arena, "Fin", NULL, NULL, 0, 7, true, NULL);
  ny_type_t *symbolic_index =
      ny_type_apply_bounded(&arena, "Fin", NULL, NULL, 0, 0, false, "Count");
  ny_type_t *pointer_payload = ny_type_concrete(&arena, "*int");
  ny_type_t *nullable_payload = ny_type_concrete(&arena, "?int");
  ny_type_t *struct_payload = ny_type_concrete(&arena, "struct project.Widget");
  ny_type_t *enum_payload = ny_type_concrete(&arena, "enum project.State");
  ny_type_t *layout_payload = ny_type_concrete(&arena, "layout project.Header");
  ny_type_t *opaque_payload =
      ny_type_concrete(&arena, "domain/type with spaces%and<delimiters>");
  ny_type_t *opaque_generic =
      ny_type_apply(&arena, "Box", opaque_payload, NULL, 1);
  ny_type_t *ambiguous_concrete = ny_type_concrete(&arena, "Fin<7>");
  ny_type_t *generic_row = ny_type_row(
      &arena, "effect.with.generic.payload", payload_type,
      ny_type_row(
          &arena, "effect.with.numeric.index", numeric_index,
          ny_type_row(
              &arena, "effect.with.symbolic.index", symbolic_index,
              ny_type_row(
                  &arena, "effect.with.pointer.payload", pointer_payload,
                  ny_type_row(
                      &arena, "effect.with.nullable.payload", nullable_payload,
                      ny_type_row(
                          &arena, "effect.with.struct.payload", struct_payload,
                          ny_type_row(
                              &arena, "effect.with.enum.payload", enum_payload,
                              ny_type_row(
                                  &arena, "effect.with.layout.payload",
                                  layout_payload,
                                  ny_type_row(
                                      &arena, "effect.with.opaque.payload",
                                      opaque_generic,
                                      ny_type_row(
                                          &arena,
                                          "effect.with.ambiguous.payload",
                                          ambiguous_concrete, NULL))))))))));
  char *left_text = ny_type_effect_row_serialize(left);
  char *right_text = ny_type_effect_row_serialize(right);
  char *open_a_text = ny_type_effect_row_serialize(open_a);
  char *open_b_text = ny_type_effect_row_serialize(open_b);
  char *generic_text = ny_type_effect_row_serialize(generic_row);
  ny_type_arena_t decoded_arena;
  ny_type_arena_init(&decoded_arena);
  ny_type_t *decoded_closed =
      left_text ? ny_type_effect_row_deserialize(&decoded_arena, left_text)
                : NULL;
  ny_type_t *decoded_open =
      open_a_text ? ny_type_effect_row_deserialize(&decoded_arena, open_a_text)
                  : NULL;
  ny_type_t *decoded_open_again =
      open_a_text ? ny_type_effect_row_deserialize(&decoded_arena, open_a_text)
                  : NULL;
  ny_type_t *decoded_generic =
      generic_text
          ? ny_type_effect_row_deserialize(&decoded_arena, generic_text)
          : NULL;
  ny_type_t *decoded_open_tail =
      decoded_open && decoded_open->kind == NY_TYPE_ROW
          ? ny_type_find(decoded_open->as.row.tail)
          : NULL;
  ny_type_t *decoded_open_again_tail =
      decoded_open_again && decoded_open_again->kind == NY_TYPE_ROW
          ? ny_type_find(decoded_open_again->as.row.tail)
          : NULL;
  bool imports_fresh_open_tails =
      decoded_open_tail && decoded_open_again_tail &&
      decoded_open_tail != decoded_open_again_tail &&
      decoded_open_tail->kind == NY_TYPE_EFFECT_VAR &&
      decoded_open_again_tail->kind == NY_TYPE_EFFECT_VAR;
  ny_type_t *decoded_conflict = ny_type_effect_row_deserialize(
      &decoded_arena, "ny.effect-row.v1{std.effects.io=effect}");
  char *decoded_closed_text =
      decoded_closed ? ny_type_effect_row_serialize(decoded_closed) : NULL;
  char *decoded_open_text =
      decoded_open ? ny_type_effect_row_serialize(decoded_open) : NULL;
  char *decoded_generic_text =
      decoded_generic ? ny_type_effect_row_serialize(decoded_generic) : NULL;
  bool generic_roundtrip = generic_text && decoded_generic_text &&
                           strcmp(generic_text, decoded_generic_text) == 0;
  bool generic_unifies =
      decoded_generic && ny_type_unify(generic_row, decoded_generic);
  bool stable =
      left_text && right_text && open_a_text && open_b_text &&
      strcmp(left_text, right_text) == 0 &&
      strcmp(open_a_text, open_b_text) == 0 &&
      strstr(open_a_text, "|r0") != NULL && decoded_closed_text &&
      strcmp(left_text, decoded_closed_text) == 0 && decoded_open_text &&
      strcmp(open_a_text, decoded_open_text) == 0 && imports_fresh_open_tails &&
      generic_roundtrip && generic_unifies &&
      ny_type_effect_row_deserialize(&decoded_arena,
                                     "ny.effect-row.v1{bad%2=unit}") == NULL &&
      ny_type_effect_row_deserialize(
          &decoded_arena, "ny.effect-row.v1{a=concrete%236}") == NULL &&
      ny_type_effect_row_deserialize(
          &decoded_arena, "ny.effect-row.v1{a=concrete%2300}") == NULL &&
      ny_type_effect_row_deserialize(&decoded_arena,
                                     "ny.effect-row.v1{a%61=unit}") == NULL &&
      ny_type_effect_row_deserialize(
          &decoded_arena, "ny.effect-row.v1{b=unit,a=unit}") == NULL &&
      ny_type_effect_row_deserialize(&decoded_arena,
                                     "ny.effect-row.v1{a=unit|r1}") == NULL &&
      ny_type_effect_row_deserialize(&decoded_arena,
                                     "ny.effect-row.v1{a=unit,|r0}") == NULL &&
      ny_type_unify(decoded_open, decoded_closed) && decoded_conflict &&
      !ny_type_unify(decoded_open, decoded_conflict) &&
      ny_type_effect_row_serialize(unstable_field) == NULL;
  if (!stable)
    fprintf(stderr,
            "[row-algebra] effect row serialization was not stable "
            "(generic roundtrip=%d unifies=%d, encoded='%s' decoded='%s')\n",
            generic_roundtrip, generic_unifies,
            generic_text ? generic_text : "<null>",
            decoded_generic_text ? decoded_generic_text : "<null>");
  free(decoded_closed_text);
  free(decoded_open_text);
  free(decoded_generic_text);
  ny_type_arena_reset(&decoded_arena);
  free(left_text);
  free(right_text);
  free(open_a_text);
  free(open_b_text);
  free(generic_text);
  ny_type_arena_reset(&arena);
  return stable;
}

static bool check_row_composition_preserves_open_tails(void) {
  ny_type_arena_t arena;
  ny_type_arena_init(&arena);
  ny_type_t *field = ny_type_concrete(&arena, "effect");
  ny_type_t *tail = ny_type_effect_var(&arena);
  ny_type_t *open = ny_type_row(&arena, "alpha", field, tail);
  ny_type_t *closed = ny_type_row(&arena, "beta", field, NULL);
  ny_subst_t *subst = ny_subst_new(16);
  ny_type_t *composed = ny_type_row_compose(&arena, subst, open, closed);
  ny_type_t *reverse = ny_type_row_compose(&arena, subst, closed, open);
  bool valid = composed && reverse &&
               ny_type_row_has_label(composed, "alpha") &&
               ny_type_row_has_label(composed, "beta") &&
               ny_type_row_has_label(reverse, "alpha") &&
               ny_type_row_has_label(reverse, "beta") &&
               ny_type_find(composed->as.row.tail->as.row.tail) == tail;
  ny_type_t *other_tail = ny_type_effect_var(&arena);
  ny_type_t *other_open = ny_type_row(&arena, "gamma", field, other_tail);
  ny_type_t *both_open = ny_type_row_compose(&arena, subst, open, other_open);
  ny_type_t *tail_a = ny_type_find(tail);
  ny_type_t *tail_b = ny_type_find(other_tail);
  bool composes_two_tails = both_open &&
                            ny_type_row_has_label(both_open, "alpha") &&
                            ny_type_row_has_label(both_open, "gamma") &&
                            ny_type_row_has_label(tail_a, "gamma") &&
                            ny_type_row_has_label(tail_b, "alpha");
  ny_type_t *duplicate = ny_type_row(&arena, "beta", field,
                                     ny_type_row(&arena, "beta", field, NULL));
  ny_type_t *deduplicated =
      ny_type_row_compose(&arena, subst, closed, duplicate);
  ny_type_t *other_field = ny_type_concrete(&arena, "other-effect");
  ny_type_t *conflicting = ny_type_row(&arena, "beta", other_field, NULL);
  bool duplicate_eliminated =
      deduplicated && row_label_count(deduplicated, "beta") == 1;
  bool conflicting_fields_rejected =
      ny_type_row_compose(&arena, subst, closed, conflicting) == NULL;
  bool tail_preserved =
      composed && composed->as.row.tail && composed->as.row.tail->as.row.tail &&
      ny_type_find(composed->as.row.tail->as.row.tail) == tail;
  ny_subst_free(subst);
  ny_type_arena_reset(&arena);
  if (!valid || !composes_two_tails || !duplicate_eliminated ||
      !conflicting_fields_rejected) {
    fprintf(stderr,
            "[row-algebra] row composition invariant failed "
            "(valid=%d tail=%d dual=%d dedupe=%d conflict=%d)\n",
            valid, tail_preserved, composes_two_tails, duplicate_eliminated,
            conflicting_fields_rejected);
  }
  return valid && composes_two_tails && duplicate_eliminated &&
         conflicting_fields_rejected;
}

static bool check_structural_type_identity_ignores_variable_print_ids(void) {
  ny_type_arena_t left_arena;
  ny_type_arena_t right_arena;
  ny_type_arena_init(&left_arena);
  ny_type_arena_init(&right_arena);
  ny_type_t *left_var = ny_type_var(&left_arena);
  ny_type_t *right_var = ny_type_var(&right_arena);
  ny_type_t *left_effect = ny_type_row(&left_arena, "effect", left_var, NULL);
  ny_type_t *right_effect =
      ny_type_row(&right_arena, "effect", right_var, NULL);
  ny_type_t *left_concrete =
      ny_type_apply(&left_arena, "Result", ny_type_concrete(&left_arena, "i64"),
                    ny_type_concrete(&left_arena, "unit"), 2);
  ny_type_t *right_concrete = ny_type_apply(
      &right_arena, "Result", ny_type_concrete(&right_arena, "i64"),
      ny_type_concrete(&right_arena, "unit"), 2);
  char *left_var_text = ny_type_to_string(left_var);
  char *right_var_text = ny_type_to_string(right_var);
  bool same_printed_id = left_var_text && right_var_text &&
                         strcmp(left_var_text, right_var_text) == 0;
  bool valid =
      left_var && right_var && left_effect && right_effect && same_printed_id &&
      !ny_type_structurally_equal(left_effect, right_effect) && left_concrete &&
      right_concrete &&
      ny_type_structurally_equal(left_concrete, right_concrete) &&
      ny_type_structurally_equal(NULL, ny_type_concrete(&left_arena, "any")) &&
      !ny_type_structurally_equal(NULL,
                                  ny_type_row(&left_arena, NULL, NULL, NULL)) &&
      ny_type_structurally_equal(left_var, left_var);
  free(left_var_text);
  free(right_var_text);
  ny_type_arena_reset(&left_arena);
  ny_type_arena_reset(&right_arena);
  if (!valid)
    fprintf(stderr,
            "[row-algebra] structural type identity confused arena-local "
            "variable ids or rejected equal constructors\n");
  return valid;
}

static bool check_canonical_proposition_roundtrip(void) {
  static const char canonical[] = "binary:&&(binary:<=(name:value,int:5),"
                                  "unary:!(binary:==(name:value,int:0)))";
  arena_t arena = {0};
  expr_t *decoded = ny_proof_expr_from_canonical(&arena, canonical);
  char *encoded = decoded ? ny_proof_type_from_expr(decoded) : NULL;
  expr_t *decoded_string = ny_proof_expr_from_canonical(
      &arena, "binary:==(str:\"line\\n\",str:\"line\\n\")");
  char *encoded_string =
      decoded_string ? ny_proof_type_from_expr(decoded_string) : NULL;
  expr_t *decoded_float =
      ny_proof_expr_from_canonical(&arena, "binary:<(float:1.25,float:2.5)");
  char *encoded_float =
      decoded_float ? ny_proof_type_from_expr(decoded_float) : NULL;
  bool valid =
      encoded &&
      strcmp(encoded, "proof<binary:&&(binary:<=(name:value,int:5),"
                      "unary:!(binary:==(int:0,name:value)))>") == 0 &&
      encoded_string &&
      strcmp(encoded_string,
             "proof<binary:==(str:\"line\\n\",str:\"line\\n\")>") == 0 &&
      encoded_float &&
      strcmp(encoded_float, "proof<binary:<(float:1.25,float:2.5)>") == 0 &&
      ny_proof_expr_from_canonical(&arena, "binary:broken") == NULL &&
      ny_proof_expr_from_canonical(&arena, "binary:<(name:x)") == NULL;
  arena_free(&arena);
  if (!valid)
    fprintf(stderr,
            "[row-algebra] canonical proposition import did not round-trip "
            "or failed to reject malformed input (decoded=%s encoded=%s)\n",
            decoded ? "yes" : "no", encoded ? encoded : "<null>");
  free(encoded);
  free(encoded_string);
  free(encoded_float);
  return valid;
}

int ny_type_row_algebra_test(void) {
  if (!check_closed_sets() || !check_duplicate_elimination() ||
      !check_open_row_unification() || !check_occurs_rejection() ||
      !check_closed_tail_stays_closed() || !check_distinct_open_tails() ||
      !check_row_compose_unifies_payloads() ||
      !check_builtin_effect_masks_are_exact() ||
      !check_capability_effect_identity() ||
      !check_capability_rows_are_distinct_and_serializable() ||
      !check_module_owned_capability_identity() ||
      !check_external_effect_variables_are_isolated() ||
      !check_effect_row_serialization_is_stable() ||
      !check_row_composition_preserves_open_tails() ||
      !check_structural_type_identity_ignores_variable_print_ids() ||
      !check_canonical_proposition_roundtrip())
    return 1;
  printf("typing row algebra test: closed sets, duplicate normalization, "
         "open-tail residuals, occurs check, exact effect masks, capability "
         "identity, isolated signature variables, stable serialization with "
         "fresh import tails, structural payload identity, payload-aware row "
         "composition, and canonical proposition AST transport passed\n");
  return 0;
}
