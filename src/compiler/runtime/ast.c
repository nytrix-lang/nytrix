#include "ast_json.h"
#include "parser.h"
#include <string.h>
#include <stdlib.h>

extern int64_t rt_malloc(int64_t n);

int64_t rt_parse_ast(int64_t source_ptr) {
	const char *source = (const char *)source_ptr;
	if (!source) return 0;
	nt_parser parser;
	nt_parser_init(&parser, source, "<parse_ast>");
	nt_program prog = nt_parse_program(&parser);
	char *json = nt_ast_to_json(&prog);
	if (!json) {
		nt_arena_free(parser.arena);
		return 0;
	}
	size_t len = strlen(json);
	int64_t tagged_size = ((int64_t)(len + 1) << 1) | 1;
	int64_t res = rt_malloc(tagged_size);
	*(int64_t *)((char *)res - 8) = 241; // TAG_STR (241)
	*(int64_t *)((char *)res - 16) = ((int64_t)len << 1) | 1; // Length
	memcpy((void *)res, json, len + 1);
	nt_arena_free(parser.arena);
	// free(json); // json is likely in arena
	return res;
}
