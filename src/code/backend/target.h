#ifndef NY_BACKEND_TARGET_H
#define NY_BACKEND_TARGET_H

#include <nyr/target.h>

bool nyr_target_info_init(ny_native_target_info_t *info,
                          const nyr_target_request_t *request);
bool nyr_target_is_host_executable(const nyr_target_request_t *request);

#endif
