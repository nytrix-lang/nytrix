#ifndef NY_NATIVE_IR_API_INTERNAL_H
#define NY_NATIVE_IR_API_INTERNAL_H

#include "code/backend/internal.h"

void nyr_api_error(char *err, size_t err_len, const char *message);
void nyr_api_clear_error(char *err, size_t err_len);
bool nyr_api_target_has(const ny_native_target_info_t *target,
                        unsigned capability);
bool nyr_target_info_for_triple(const char *triple,
                                ny_native_target_info_t *info, char *err,
                                size_t err_len);

#endif
