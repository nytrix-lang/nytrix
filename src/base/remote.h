#pragma once

/* Returns an owned cache path, or NULL when the download fails. */
char *ny_resolve_remote_module(const char *url);
