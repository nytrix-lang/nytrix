/* Line-input API used by the Ny interactive REPL. */
#ifndef NYTRIX_REPL_READ_H
#define NYTRIX_REPL_READ_H

char *ny_readline(const char *prompt);
extern const char *ny_readline_prefill;
extern int ny_readline_allow_pop;
extern int ny_readline_pop_request;
void ny_readline_add_history(const char *line);
void ny_readline_init(void);
int ny_readline_read_history(const char *path);
int ny_readline_write_history(const char *path);
void ny_readline_stifle_history(int max);
void ny_readline_prepare_console(void);
int ny_readline_vt_output_ok(void);

#endif
