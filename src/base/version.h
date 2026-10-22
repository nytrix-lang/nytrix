#pragma once

/*
 * Machine-readable build identity for stale-pair detection (audit A6).
 * version:  "<VERSION> head=<HEAD> dirty=<0|1> build=<N> std=<SHA12>:<FILES>"
 * Every binary embedding this reports the commit it was built from, whether
 * the worktree was dirty, and the hash of the stdlib sources it consumed,
 * so the `ny` launcher can warn when it dispatches to a stale `ny-full` or
 * a skewed stdlib bundle instead of silently running them.
 */
const char *ny_version_id(void);

/* Prints ny_version_id() with a trailing newline; returns 0. */
int ny_print_version_id(void);
