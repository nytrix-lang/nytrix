#include <nytrix/nytrix.h>

#include "code/lang/runtime/gc.h"
#include "code/lang/runtime/shared.h"

#include <stdio.h>
#include <string.h>

extern int64_t rt_append(int64_t list, int64_t value);

int main(void) {
  char err[256] = {0};
  if (!nytrix_embed_startup(err, sizeof(err))) {
    fprintf(stderr, "embedding startup failed: %s\n", err);
    return 1;
  }

  int64_t original = nyGcAlloc(32);
  if (!original || !rt_gc_owns_pointer((uintptr_t)original)) {
    fputs("GC did not allocate the fixture object\n", stderr);
    return 1;
  }

  nytrix_value_handle_t *handle = NULL;
  if (!nytrix_value_handle_create(original, &handle, err, sizeof(err))) {
    fprintf(stderr, "could not create root handle: %s\n", err);
    return 1;
  }

  if (nytrix_embed_shutdown(err, sizeof(err))) {
    fputs("shutdown succeeded while a value handle was live\n", stderr);
    nytrix_value_handle_release(handle);
    return 1;
  }

  nyGcTriggerMajor();
  nytrix_value_t moved = 0;
  if (!nytrix_value_handle_get(handle, &moved, err, sizeof(err)) || !moved ||
      !rt_gc_owns_pointer((uintptr_t)moved)) {
    fprintf(stderr, "rooted GC value did not survive collection: %s\n", err);
    nytrix_value_handle_release(handle);
    return 1;
  }

  /*
   * Raw value handles retain aggregate Ny values as well as the scalar
   * layouts covered by the typed accessors. Build a tuple through the runtime
   * list API and verify its contents after a moving collection.
   */
  int64_t aggregate = rt_list_new_raw(2);
  if (!aggregate ||
      (aggregate = rt_append(aggregate, rt_tag_v(17))) == 0 ||
      (aggregate = rt_append(aggregate, rt_tag_v(-29))) == 0) {
    fputs("could not build aggregate embedding value\n", stderr);
    nytrix_value_handle_release(handle);
    return 1;
  }
  aggregate = rt_list_as_tuple(aggregate);
  nytrix_value_handle_t *aggregate_handle = NULL;
  if (!nytrix_value_handle_create(aggregate, &aggregate_handle, err,
                                  sizeof(err))) {
    fprintf(stderr, "could not root aggregate value: %s\n", err);
    nytrix_value_handle_release(handle);
    return 1;
  }
  nyGcTriggerMajor();
  nytrix_value_t aggregate_after_gc = 0;
  int64_t aggregate_length = 0;
  int64_t aggregate_first = 0;
  int64_t aggregate_second = 0;
  if (!nytrix_value_handle_get(aggregate_handle, &aggregate_after_gc, err,
                               sizeof(err)) ||
      !rt_bigint_to_i64_checked(rt_list_len(aggregate_after_gc),
                                &aggregate_length) ||
      aggregate_length != 2 ||
      !rt_bigint_to_i64_checked(
          rt_value_get_index_raw(aggregate_after_gc, 0, NY_IMM_NIL),
          &aggregate_first) ||
      !rt_bigint_to_i64_checked(
          rt_value_get_index_raw(aggregate_after_gc, 1, NY_IMM_NIL),
          &aggregate_second) ||
      aggregate_first != 17 || aggregate_second != -29) {
    fprintf(stderr, "aggregate handle did not survive collection: %s\n", err);
    nytrix_value_handle_release(aggregate_handle);
    nytrix_value_handle_release(handle);
    return 1;
  }
  nytrix_value_handle_release(aggregate_handle);
  (void)rt_tbuf_release_handle(aggregate);

  /*
   * A raw aggregate handle owns the root for the whole reachable value graph.
   * Exercise nested collections and a non-small string, then release the
   * child handles before collecting so only the outer aggregate can keep the
   * values alive.
   */
  static const char nested_text[] =
      "an embedded string longer than the small-string representation";
  nytrix_value_handle_t *text_handle = NULL;
  nytrix_value_handle_t *nested_handle = NULL;
  nytrix_value_handle_t *dict_handle = NULL;
  nytrix_value_handle_t *outer_handle = NULL;
  int64_t text_value = rt_alloc_string_len(nested_text, sizeof(nested_text) - 1);
  if (!nytrix_value_handle_create(text_value, &text_handle, err, sizeof(err))) {
    fprintf(stderr, "could not root nested embedding values: %s\n", err);
    nytrix_value_handle_release(text_handle);
    nytrix_value_handle_release(handle);
    return 1;
  }
  int64_t nested_list = rt_list_new_raw(1);
  if (!nytrix_value_handle_create(nested_list, &nested_handle, err,
                                  sizeof(err))) {
    fprintf(stderr, "could not root nested embedding list: %s\n", err);
    nytrix_value_handle_release(text_handle);
    nytrix_value_handle_release(handle);
    return 1;
  }
  int64_t text_current = 0;
  if (!nytrix_value_handle_get(nested_handle, &nested_list, err, sizeof(err)) ||
      !nytrix_value_handle_get(text_handle, &text_current, err, sizeof(err)) ||
      (nested_list = rt_append(nested_list, text_current)) == 0 ||
      !nytrix_value_handle_set(nested_handle, nested_list, err, sizeof(err))) {
    fprintf(stderr, "could not build nested embedding list: %s\n", err);
    nytrix_value_handle_release(nested_handle);
    nytrix_value_handle_release(text_handle);
    nytrix_value_handle_release(handle);
    return 1;
  }
  int64_t nested_dict = rt_dict_new_raw(1);
  int64_t child_key = rt_alloc_string_len("child", 5);
  int64_t nested_current = 0;
  if (!nytrix_value_handle_create(nested_dict, &dict_handle, err, sizeof(err)) ||
      !nytrix_value_handle_get(nested_handle, &nested_current, err,
                               sizeof(err)) ||
      (nested_dict = rt_dict_set_str_raw(nested_dict, child_key,
                                         nested_current)) == 0 ||
      !nytrix_value_handle_set(dict_handle, nested_dict, err, sizeof(err))) {
    fprintf(stderr, "could not build nested embedding dictionary: %s\n", err);
    nytrix_value_handle_release(dict_handle);
    nytrix_value_handle_release(nested_handle);
    nytrix_value_handle_release(text_handle);
    nytrix_value_handle_release(handle);
    return 1;
  }
  int64_t outer_list = rt_list_new_raw(2);
  int64_t dict_current = 0;
  if (!nytrix_value_handle_create(outer_list, &outer_handle, err, sizeof(err)) ||
      !nytrix_value_handle_get(nested_handle, &nested_current, err,
                               sizeof(err)) ||
      !nytrix_value_handle_get(outer_handle, &outer_list, err, sizeof(err)) ||
      (outer_list = rt_append(outer_list, nested_current)) == 0 ||
      !nytrix_value_handle_set(outer_handle, outer_list, err, sizeof(err)) ||
      !nytrix_value_handle_get(dict_handle, &dict_current, err, sizeof(err)) ||
      !nytrix_value_handle_get(outer_handle, &outer_list, err, sizeof(err)) ||
      (outer_list = rt_append(outer_list, dict_current)) == 0 ||
      !nytrix_value_handle_set(outer_handle, outer_list, err, sizeof(err))) {
    fprintf(stderr, "could not build outer embedding aggregate: %s\n", err);
    nytrix_value_handle_release(outer_handle);
    nytrix_value_handle_release(dict_handle);
    nytrix_value_handle_release(nested_handle);
    nytrix_value_handle_release(text_handle);
    nytrix_value_handle_release(handle);
    return 1;
  }
  nytrix_value_handle_release(dict_handle);
  dict_handle = NULL;
  nytrix_value_handle_release(nested_handle);
  nested_handle = NULL;
  nytrix_value_handle_release(text_handle);
  text_handle = NULL;
  nyGcTriggerMajor();
  int64_t outer_after_gc = 0;
  int64_t nested_after_gc = 0;
  int64_t dict_after_gc = 0;
  int64_t nested_text_after_gc = 0;
  int64_t dict_child_after_gc = 0;
  if (!nytrix_value_handle_get(outer_handle, &outer_after_gc, err,
                               sizeof(err)) ||
      !rt_bigint_to_i64_checked(rt_list_len(outer_after_gc), &aggregate_length) ||
      aggregate_length != 2 ||
      (nested_after_gc = rt_value_get_index_raw(outer_after_gc, 0,
                                                NY_IMM_NIL)) == NY_IMM_NIL ||
      (dict_after_gc = rt_value_get_index_raw(outer_after_gc, 1,
                                              NY_IMM_NIL)) == NY_IMM_NIL ||
      (nested_text_after_gc =
           rt_value_get_index_raw(nested_after_gc, 0, NY_IMM_NIL)) ==
          NY_IMM_NIL ||
      (dict_child_after_gc =
           rt_dict_get_str_raw(dict_after_gc, child_key, NY_IMM_NIL)) ==
          NY_IMM_NIL ||
      dict_child_after_gc != nested_after_gc || !is_v_str(nested_text_after_gc) ||
      rt_tagged_str_len(nested_text_after_gc) != sizeof(nested_text) - 1) {
    fprintf(stderr, "nested aggregate graph did not survive collection: %s\n",
            err);
    nytrix_value_handle_release(outer_handle);
    nytrix_value_handle_release(handle);
    return 1;
  }
  nytrix_value_handle_release(outer_handle);
  (void)rt_tbuf_release_handle(outer_after_gc);

  const int64_t integers[] = {INT64_MIN, -1, 0, 1, INT64_MAX};
  for (size_t i = 0; i < sizeof(integers) / sizeof(integers[0]); ++i) {
    int64_t roundtrip = 0;
    if (!nytrix_value_handle_set_i64(handle, integers[i], err, sizeof(err)) ||
        !nytrix_value_handle_get_i64(handle, &roundtrip, err, sizeof(err)) ||
        roundtrip != integers[i]) {
      fprintf(stderr, "i64 marshalling failed: %s\n", err);
      nytrix_value_handle_release(handle);
      return 1;
    }
  }

  bool boolean = false;
  int64_t wrong_type = 0;
  if (!nytrix_value_handle_set_bool(handle, true, err, sizeof(err)) ||
      !nytrix_value_handle_get_bool(handle, &boolean, err, sizeof(err)) ||
      !boolean ||
      !nytrix_value_handle_set_bool(handle, false, err, sizeof(err)) ||
      !nytrix_value_handle_get_bool(handle, &boolean, err, sizeof(err)) ||
      boolean ||
      nytrix_value_handle_get_i64(handle, &wrong_type, err, sizeof(err))) {
    fprintf(stderr, "boolean marshalling/type rejection failed: %s\n", err);
    nytrix_value_handle_release(handle);
    return 1;
  }

  double floating = 0.0;
  if (!nytrix_value_handle_set_f64(handle, -17.25, err, sizeof(err)) ||
      !nytrix_value_handle_get_f64(handle, &floating, err, sizeof(err)) ||
      floating != -17.25) {
    fprintf(stderr, "f64 marshalling failed: %s\n", err);
    nytrix_value_handle_release(handle);
    return 1;
  }

  const char bytes[] = {'A', '\0', 'B', (char)0xff};
  size_t length = 0;
  char copied[sizeof(bytes)] = {0};
  if (!nytrix_value_handle_set_string(handle, bytes, sizeof(bytes), err,
                                      sizeof(err)) ||
      !nytrix_value_handle_get_string(handle, NULL, 0, &length, err,
                                      sizeof(err)) ||
      length != sizeof(bytes) ||
      nytrix_value_handle_get_string(handle, copied, sizeof(copied) - 1,
                                     &length, err, sizeof(err)) ||
      !nytrix_value_handle_get_string(handle, copied, sizeof(copied), &length,
                                      err, sizeof(err)) ||
      length != sizeof(bytes) || memcmp(copied, bytes, sizeof(bytes)) != 0) {
    fprintf(stderr, "byte-string marshalling failed: %s\n", err);
    nytrix_value_handle_release(handle);
    return 1;
  }

  if (!nytrix_value_handle_set_nil(handle, err, sizeof(err)) ||
      !nytrix_value_handle_get(handle, &moved, err, sizeof(err)) ||
      moved != 0) {
    fprintf(stderr, "nil marshalling failed: %s\n", err);
    nytrix_value_handle_release(handle);
    return 1;
  }

  nytrix_value_handle_release(handle);
  if (!nytrix_embed_shutdown(err, sizeof(err)) ||
      !nytrix_embed_startup(err, sizeof(err))) {
    fprintf(stderr, "embedding lifecycle roundtrip failed: %s\n", err);
    return 1;
  }
  handle = NULL;
  if (!nytrix_value_handle_create(nyGcAlloc(32), &handle, err, sizeof(err)) ||
      !nytrix_value_handle_set_f64(handle, 1.5, err, sizeof(err)) ||
      !nytrix_value_handle_get_f64(handle, &floating, err, sizeof(err)) ||
      floating != 1.5) {
    fprintf(stderr, "runtime restart failed: %s\n", err);
    nytrix_value_handle_release(handle);
    return 1;
  }
  nytrix_value_handle_release(handle);
  if (!nytrix_embed_shutdown(err, sizeof(err))) {
    fprintf(stderr, "second embedding shutdown failed: %s\n", err);
    return 1;
  }
  puts("embedding lifecycle and host-value marshalling passed");
  return 0;
}
