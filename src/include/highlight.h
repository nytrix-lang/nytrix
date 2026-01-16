#ifndef NYTRIX_REPL_HIGHLIGHT_H
#define NYTRIX_REPL_HIGHLIGHT_H

void repl_highlight_line(const char *line);
void repl_init_highlighting(void);
void repl_display_highlighted(const char *line);
void repl_redisplay_with_highlight(void);

#endif // NYTRIX_REPL_HIGHLIGHT_H
