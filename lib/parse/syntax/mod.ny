;; Keywords: syntax xml c typescript ts cmake build-system python nytrix language yaml yml bash shell assembly asm markdown md lua json javascript js html markup helpers
;; Syntax-tokenizer facade for Nytrix, common programming languages, markup, and config formats.
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
   ".js", ".jsx", ".mjs" -> "javascript"
   ".ts", ".tsx" -> "typescript"
   ".lua" -> "lua"
   ".sh", ".bash", ".zsh", ".fish" -> "bash"
   ".cmake" -> "cmake"
   ".yaml", ".yml" -> "yaml"
   ".xml" -> "xml"
   ".html", ".htm" -> "html"
   ".md", ".markdown" -> "markdown"
   ".s", ".S", ".asm" -> "assembly"
   ".json" -> "json"
}

fn nytrix_tokenize(str: source, list: out_tokens): list { _nytrix_mod.tokenize(source, out_tokens) }

fn c_tokenize(str: source, list: out_tokens): list { _c_mod.tokenize(source, out_tokens) }

fn python_tokenize(str: source, list: out_tokens): list { _python_mod.tokenize(source, out_tokens) }

fn javascript_tokenize(str: source, list: out_tokens): list { _javascript_mod.tokenize(source, out_tokens) }

fn typescript_tokenize(str: source, list: out_tokens): list { _typescript_mod.tokenize(source, out_tokens) }

fn lua_tokenize(str: source, list: out_tokens): list { _lua_mod.tokenize(source, out_tokens) }

fn bash_tokenize(str: source, list: out_tokens): list { _bash_mod.tokenize(source, out_tokens) }

fn cmake_tokenize(str: source, list: out_tokens): list { _cmake_mod.tokenize(source, out_tokens) }

fn yaml_tokenize(str: source, list: out_tokens): list { _yaml_mod.tokenize(source, out_tokens) }

fn xml_tokenize(str: source, list: out_tokens): list { _xml_mod.tokenize(source, out_tokens) }

fn html_tokenize(str: source, list: out_tokens): list { _html_mod.tokenize(source, out_tokens) }

fn markdown_tokenize(str: source, list: out_tokens): list { _markdown_mod.tokenize(source, out_tokens) }

fn assembly_tokenize(str: source, list: out_tokens): list { _assembly_mod.tokenize(source, out_tokens) }

fn json_tokenize(str: source, list: out_tokens): list { _json_mod.tokenize(source, out_tokens) }

fn detect_language(?str: filename): str {
   if(!filename || !is_str(filename)){ return "text" }
   if(filename == "CMakeLists.txt"){ return "cmake" }
   def dot = str.find_last(filename, ".")
   if(dot < 0){ return "text" }
   def ext = str.str_slice(filename, dot, filename.len)
   comptime match SyntaxLangByExt(ext, "text")
}

fn tokenize_auto(str: source, ?str: filename, list: out_tokens): list {
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
