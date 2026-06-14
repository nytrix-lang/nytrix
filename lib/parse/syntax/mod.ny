;; Keywords: syntax xml c typescript ts cmake build-system python nytrix language yaml yml bash shell assembly asm markdown md lua json javascript js html markup helpers parse highlight
;; Syntax-tokenizer facade for Nytrix, common programming languages, markup, and config formats.
;; References:
;; - std.parse
;; - std.parse.syntax.helpers
module std.parse.syntax(TOK_KEYWORD, TOK_TYPE, TOK_STRING, TOK_NUMBER, TOK_COMMENT, TOK_FUNCTION, TOK_OPERATOR, TOK_PUNCT, TOK_VARIABLE, TOK_CONSTANT, TOK_PREPROC, TOK_PARAM, TOK_STRUCT, TOK_PROPERTY, TOK_TEXT, TOK_LABEL, TOK_REGISTER, TOK_DIRECTIVE, TOK_TAG, TOK_ATTR, TOK_KEY, TOK_VALUE, nytrix_tokenize, c_tokenize, python_tokenize, javascript_tokenize, typescript_tokenize, lua_tokenize, bash_tokenize, cmake_tokenize, yaml_tokenize, xml_tokenize, html_tokenize, markdown_tokenize, assembly_tokenize, json_tokenize, tokenize_auto, detect_language)
use std.core
use std.core.str as str
use std.parse.syntax.helpers as _h
use std.parse.syntax.nytrix as _nytrix_mod
use std.parse.syntax.c as _c_mod
use std.parse.syntax.python as _python_mod
use std.parse.syntax.javascript as _javascript_mod
use std.parse.syntax.typescript as _typescript_mod
use std.parse.syntax.lua as _lua_mod
use std.parse.syntax.bash as _bash_mod
use std.parse.syntax.cmake as _cmake_mod
use std.parse.syntax.yaml as _yaml_mod
use std.parse.syntax.xml as _xml_mod
use std.parse.syntax.html as _html_mod
use std.parse.syntax.markdown as _markdown_mod
use std.parse.syntax.assembly as _assembly_mod
use std.parse.syntax.json as _json_mod

def TOK_KEYWORD = 0
def TOK_TYPE = 1
def TOK_STRING = 2
def TOK_NUMBER = 3
def TOK_COMMENT = 4
def TOK_FUNCTION = 5
def TOK_OPERATOR = 6
def TOK_PUNCT = 7
def TOK_VARIABLE = 8
def TOK_CONSTANT = 9
def TOK_PREPROC = 10
def TOK_PARAM = 11
def TOK_STRUCT = 12
def TOK_PROPERTY = 13
def TOK_TEXT = 14
def TOK_LABEL = 15
def TOK_REGISTER = 16
def TOK_DIRECTIVE = 17
def TOK_TAG = 18
def TOK_ATTR = 19
def TOK_KEY = 20
def TOK_VALUE = 21

comptime table SyntaxLangByExt {
   ".ny" -> "nytrix"
   ".c", ".h", ".cpp", ".hpp", ".cc", ".hh", ".cxx", ".hxx" -> "c"
   ".py", ".pyw" -> "python"
   ".js", ".jsx", ".mjs", ".cjs" -> "javascript"
   ".ts", ".tsx", ".mts", ".cts" -> "typescript"
   ".lua" -> "lua"
   ".sh", ".bash", ".zsh", ".fish" -> "bash"
   ".cmake" -> "cmake"
   ".yaml", ".yml" -> "yaml"
   ".xml" -> "xml"
   ".html", ".htm" -> "html"
   ".md", ".markdown", ".mdx" -> "markdown"
   ".s", ".S", ".asm" -> "assembly"
   ".json" -> "json"
}

fn nytrix_tokenize(str source, list out_tokens) list { _nytrix_mod.tokenize(source, out_tokens) }

fn c_tokenize(str source, list out_tokens) list { _c_mod.tokenize(source, out_tokens) }

fn python_tokenize(str source, list out_tokens) list { _python_mod.tokenize(source, out_tokens) }

fn javascript_tokenize(str source, list out_tokens) list { _javascript_mod.tokenize(source, out_tokens) }

fn typescript_tokenize(str source, list out_tokens) list { _typescript_mod.tokenize(source, out_tokens) }

fn lua_tokenize(str source, list out_tokens) list { _lua_mod.tokenize(source, out_tokens) }

fn bash_tokenize(str source, list out_tokens) list { _bash_mod.tokenize(source, out_tokens) }

fn cmake_tokenize(str source, list out_tokens) list { _cmake_mod.tokenize(source, out_tokens) }

fn yaml_tokenize(str source, list out_tokens) list { _yaml_mod.tokenize(source, out_tokens) }

fn xml_tokenize(str source, list out_tokens) list { _xml_mod.tokenize(source, out_tokens) }

fn html_tokenize(str source, list out_tokens) list { _html_mod.tokenize(source, out_tokens) }

fn markdown_tokenize(str source, list out_tokens) list { _markdown_mod.tokenize(source, out_tokens) }

fn assembly_tokenize(str source, list out_tokens) list { _assembly_mod.tokenize(source, out_tokens) }

fn json_tokenize(str source, list out_tokens) list { _json_mod.tokenize(source, out_tokens) }

fn _basename_start(str filename) int {
   mut slash = -1
   mut i = 0
   while i < filename.len {
      def c = load8(filename, i)
      if c == 47 || c == 92 { slash = i }
      i += 1
   }
   slash + 1
}

fn _last_dot(str filename, int start) int {
   mut dot = -1
   mut i = start
   while i < filename.len {
      if load8(filename, i) == 46 { dot = i }
      i += 1
   }
   dot
}

fn detect_language(?str filename) str {
   "Runs the detect language operation."
   if !filename || !is_str(filename) { return "text" }
   def name_start = _basename_start(filename)
   def name = str.str_slice(filename, name_start, filename.len)
   if name == "CMakeLists.txt" { return "cmake" }
   def dot = _last_dot(filename, name_start)
   if dot < 0 { return "text" }
   def ext = str.str_slice(filename, dot, filename.len)
   comptime match SyntaxLangByExt(ext, "text")
}

fn tokenize_auto(str source, ?str filename, list out_tokens) list {
   "Runs the tokenize auto operation."
   def lang = detect_language(filename)
   case lang {
      "nytrix" -> _nytrix_mod.tokenize(source, out_tokens)
      "c" -> _c_mod.tokenize(source, out_tokens)
      "python" -> _python_mod.tokenize(source, out_tokens)
      "javascript" -> _javascript_mod.tokenize(source, out_tokens)
      "typescript" -> _typescript_mod.tokenize(source, out_tokens)
      "lua" -> _lua_mod.tokenize(source, out_tokens)
      "bash" -> _bash_mod.tokenize(source, out_tokens)
      "cmake" -> _cmake_mod.tokenize(source, out_tokens)
      "yaml" -> _yaml_mod.tokenize(source, out_tokens)
      "xml" -> _xml_mod.tokenize(source, out_tokens)
      "html" -> _html_mod.tokenize(source, out_tokens)
      "markdown" -> _markdown_mod.tokenize(source, out_tokens)
      "assembly" -> _assembly_mod.tokenize(source, out_tokens)
      "json" -> _json_mod.tokenize(source, out_tokens)
      _ -> out_tokens
   }
}

fn _has_token_kind(str source, list toks, str needle, int kind) bool {
   def pos = str.find(source, needle)
   if pos < 0 { return false }
   mut i = 0
   while i < toks.len {
      def tok = toks.get(i)
      if int(tok.get(0, -1)) == kind && int(tok.get(1, -1)) == pos { return true }
      i += 1
   }
   false
}

#main {
   def toks = tokenize_auto("cmake_minimum_required(VERSION 3.10)\nproject(x)\n", "CMakeLists.txt", list(0))
   assert(toks.len > 0 && toks.len < 64, "cmake tokenizer progress")
   def ny = "#main {\nfn draw(int x) int { gfx.draw_rect(x) return WIDTH }\n}\n"
   def ny_toks = tokenize_auto(ny, "/tmp/probe.ny", list(0))
   assert(_has_token_kind(ny, ny_toks, "#main", TOK_DIRECTIVE), "nytrix directive token")
   assert(_has_token_kind(ny, ny_toks, "draw", TOK_FUNCTION), "nytrix declared function token")
   assert(_has_token_kind(ny, ny_toks, "draw_rect", TOK_FUNCTION), "nytrix property call token")
   assert(_has_token_kind(ny, ny_toks, "WIDTH", TOK_CONSTANT), "nytrix constant token")
   def sig = "fn paint(widget canvas, int count=0) result { return count }\n"
   def sig_toks = tokenize_auto(sig, "signature.ny", list(0))
   assert(_has_token_kind(sig, sig_toks, "widget", TOK_TYPE), "nytrix type-first custom param type")
   assert(_has_token_kind(sig, sig_toks, "canvas", TOK_PARAM), "nytrix type-first param name")
   assert(_has_token_kind(sig, sig_toks, "result", TOK_TYPE), "nytrix suffix return type")
   def untyped = "fn id(solo_param) any { return solo_param }\n"
   def untyped_toks = tokenize_auto(untyped, "untyped.ny", list(0))
   assert(_has_token_kind(untyped, untyped_toks, "solo_param", TOK_PARAM), "nytrix untyped param remains a param")
   def js = "obj.method(value)\n"
   def js_toks = tokenize_auto(js, "/tmp/x.y/app.cjs", list(0))
   assert(detect_language("/tmp/x.y/app.cjs") == "javascript", "basename extension language detect")
   assert(_has_token_kind(js, js_toks, "method", TOK_FUNCTION), "c-like property call token")
   def json = "{\"name\":\"nytrix\",\"ok\":true}\n"
   def json_toks = tokenize_auto(json, "config.json", list(0))
   assert(_has_token_kind(json, json_toks, "\"name\"", TOK_KEY), "json key token")
   def yaml = "name: nytrix\nenabled: true\n"
   def yaml_toks = tokenize_auto(yaml, "config.yaml", list(0))
   assert(_has_token_kind(yaml, yaml_toks, "name", TOK_KEY), "yaml key token")
   assert(_has_token_kind(yaml, yaml_toks, "nytrix", TOK_VALUE), "yaml value token")
   print("✓ std.parse.syntax self-test passed")
}
