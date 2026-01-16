#include "parser.h"
// Mock LSP Server using the same parser as the compiler.
#include <ctype.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static ssize_t read_exact(int fd, void *buf, size_t len) {
	size_t total = 0;
	while (total < len) {
		ssize_t n = read(fd, (char *)buf + total, len - total);
		if (n <= 0) return total ? (ssize_t)total : n;
		total += (size_t)n;
	}
	return (ssize_t)total;
}

static ssize_t read_header_line(char *buf, size_t cap) {
	size_t idx = 0;
	while (idx + 1 < cap) {
		char c;
		ssize_t n = read_exact(STDIN_FILENO, &c, 1);
		if (n <= 0) return -1;
		if (c == '\r') continue;
		if (c == '\n') {
			buf[idx] = '\0';
			return (ssize_t)idx;
		}
		buf[idx++] = c;
	}
	buf[idx] = '\0';
	return (ssize_t)idx;
}

static char *read_message(void) {
	char line[256];
	ssize_t len = 0;
	ssize_t content_len = 0;
	while ((len = read_header_line(line, sizeof(line))) >= 0) {
		if (len == 0) break;
		if (strncasecmp(line, "Content-Length:", 15) == 0) {
			content_len = atoi(line + 15);
		}
	}
	if (content_len <= 0) return NULL;
	char *body = malloc((size_t)content_len + 1);
	if (!body) return NULL;
	if (read_exact(STDIN_FILENO, body, (size_t)content_len) <= 0) {
		free(body);
		return NULL;
	}
	body[content_len] = '\0';
	return body;
}

static char *json_decode_string(const char *start, size_t len) {
	char *out = malloc(len + 1);
	if (!out) return NULL;
	size_t o = 0;
	for (size_t i = 0; i < len; ++i) {
		char c = start[i];
		if (c == '\\' && i + 1 < len) {
			++i;
			char esc = start[i];
			switch (esc) {
			case '"': out[o++] = '"'; break;
			case '\\': out[o++] = '\\'; break;
			case '/': out[o++] = '/'; break;
			case 'b': out[o++] = '\b'; break;
			case 'f': out[o++] = '\f'; break;
			case 'n': out[o++] = '\n'; break;
			case 'r': out[o++] = '\r'; break;
			case 't': out[o++] = '\t'; break;
			default: out[o++] = esc; break;
			}
		} else {
			out[o++] = c;
		}
	}
	out[o] = '\0';
	return out;
}

static char *json_extract_string(const char *json, const char *key) {
	if (!json || !key) return NULL;
	char pattern[128];
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	const char *pos = strstr(json, pattern);
	if (!pos) return NULL;
	const char *colon = strchr(pos + strlen(pattern), ':');
	if (!colon) return NULL;
	const char *quote = strchr(colon, '"');
	if (!quote) return NULL;
	const char *start = quote + 1;
	const char *end = start;
	while (*end && (*end != '"' || *(end - 1) == '\\')) end++;
	size_t len = (size_t)(end - start);
	return json_decode_string(start, len);
}

static char *json_extract_string_near(const char *json, const char *needle, const char *key) {
	if (!json) return NULL;
	const char *section = strstr(json, needle);
	if (section) return json_extract_string(section, key);
	return json_extract_string(json, key);
}

static char *json_extract_id(const char *json) {
	if (!json) return NULL;
	const char *pos = strstr(json, "\"id\"");
	if (!pos) return NULL;
	const char *colon = strchr(pos, ':');
	if (!colon) return NULL;
	const char *p = colon + 1;
	while (*p && isspace((unsigned char)*p)) p++;
	if (*p == '"') {
		const char *start = ++p;
		while (*p && *p != '"') {
			if (*p == '\\' && p[1]) p += 2;
			else p++;
		}
		size_t len = (size_t)(p - start);
		return json_decode_string(start, len);
	}
	const char *start = p;
	while (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p)) p++;
	size_t len = (size_t)(p - start);
	char *out = malloc(len + 1);
	if (!out) return NULL;
	memcpy(out, start, len);
	out[len] = '\0';
	return out;
}

static void send_response(const char *json) {
	if (!json) return;
	char header[64];
	int body_len = (int)strlen(json);
	int header_len = snprintf(header, sizeof(header), "Content-Length: %d\r\n\r\n", body_len);
	write(STDOUT_FILENO, header, (size_t)header_len);
	write(STDOUT_FILENO, json, (size_t)body_len);
	fsync(STDOUT_FILENO);
}

static void publish_diagnostics(const char *uri, const char *message, int line, int col, bool has_error) {
	if (!uri) return;
	char body[4096];
	char escaped[512];
	size_t esc_idx = 0;
	const char *src = message ? message : "parse error";
	while (*src && esc_idx + 6 < sizeof(escaped)) {
		char c = *src++;
		switch (c) {
		case '"': escaped[esc_idx++] = '\\'; escaped[esc_idx++] = '"'; break;
		case '\\': escaped[esc_idx++] = '\\'; escaped[esc_idx++] = '\\'; break;
		case '\n': escaped[esc_idx++] = '\\'; escaped[esc_idx++] = 'n'; break;
		case '\r': escaped[esc_idx++] = '\\'; escaped[esc_idx++] = 'r'; break;
		case '\t': escaped[esc_idx++] = '\\'; escaped[esc_idx++] = 't'; break;
		default: escaped[esc_idx++] = c; break;
		}
	}
	escaped[esc_idx] = '\0';
	if (!has_error) {
		snprintf(body, sizeof(body),
			"{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":"
			"{\"uri\":\"%s\",\"diagnostics\":[]}}", uri);
		send_response(body);
		return;
	}
	snprintf(body, sizeof(body),
		"{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":"
		"{\"uri\":\"%s\",\"diagnostics\":[{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
		"\"end\":{\"line\":%d,\"character\":%d}},\"severity\":1,\"message\":\"%s\"}]}}",
		uri, line, col, line, col, escaped[0] ? escaped : "parse error");
	send_response(body);
}

static bool analyze_text(const char *text, char **out_msg, int *out_line, int *out_col) {
	if (!text) return false;
	nt_parser parser;
	nt_parser_init(&parser, text, "<lsp>");
	parser.error_limit = 0;
	nt_program prog = nt_parse_program(&parser);
	(void)prog;
	nt_program_free(&prog, parser.arena);
	if (parser.error_count == 0) {
		if (out_msg) *out_msg = NULL;
		return false;
	}
	if (out_line) *out_line = parser.last_error_line > 0 ? parser.last_error_line - 1 : 0;
	if (out_col) *out_col = parser.last_error_col > 0 ? parser.last_error_col - 1 : 0;
	if (out_msg) {
		*out_msg = strdup(parser.last_error_msg[0] ? parser.last_error_msg : "parse error");
	}
	return true;
}

static void handle_request(const char *body) {
	if (!body) return;
	char *method = json_extract_string(body, "method");
	char *id = json_extract_id(body);
	if (!method) {
		free(id);
		return;
	}
	if (strcmp(method, "initialize") == 0) {
		char response[512];
		snprintf(response, sizeof(response),
			"{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"capabilities\":{"
			"\"textDocumentSync\":2}}}", id ? id : "null");
		send_response(response);
	} else if (strcmp(method, "textDocument/didOpen") == 0 ||
			   strcmp(method, "textDocument/didChange") == 0) {
		const char *uri = json_extract_string_near(body, "\"textDocument\"", "uri");
		char *text = NULL;
		const char *changes = strstr(body, "\"contentChanges\"");
		if (changes) {
			text = json_extract_string(changes, "text");
		} else {
			text = json_extract_string_near(body, "\"textDocument\"", "text");
		}
		int line = 0, col = 0;
		char *err = NULL;
		bool has_err = analyze_text(text, &err, &line, &col);
		publish_diagnostics(uri, err, line, col, has_err);
		free(err);
		free(text);
		free((char *)uri);
	} else if (strcmp(method, "shutdown") == 0) {
		char response[128];
		snprintf(response, sizeof(response),
			"{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":null}", id ? id : "null");
		send_response(response);
	} else if (strcmp(method, "exit") == 0) {
		free(method);
		free(id);
		exit(0);
	}
	free(method);
	free(id);
}

int main(void) {
	// Ensure stdout is unbuffered for LSP.
	setlinebuf(stdout);
	while (1) {
		char *msg = read_message();
		if (!msg) break;
		handle_request(msg);
		free(msg);
	}
	return 0;
}
