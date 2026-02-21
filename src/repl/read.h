#ifndef NYTRIX_REPL_READ_H
#define NYTRIX_REPL_READ_H

/* Cross-platform raw-mode multiline readline implementation */

char *ny_readline(const char *prompt);
void ny_readline_add_history(const char *line);
void ny_readline_init(void);
int ny_readline_read_history(const char *path);
int ny_readline_write_history(const char *path);
void ny_readline_stifle_history(int max);

#endif
