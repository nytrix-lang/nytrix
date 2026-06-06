;; Keywords: syntax c parse highlight
;; C syntax highlighter
;; References:
;; - std.parse.syntax
;; - std.parse.syntax.helpers
module std.parse.syntax.c(tokenize)
use std.parse.syntax.helpers as _h

def KW = "auto;break;case;char;const;continue;default;do;double;else;enum;extern;float;for;goto;if;inline;int;long;register;restrict;return;short;signed;sizeof;static;struct;switch;typedef;union;unsigned;void;volatile;while;true;false;nullptr"
def TP = "int8_t;int16_t;int32_t;int64_t;uint8_t;uint16_t;uint32_t;uint64_t;size_t;ssize_t;ptrdiff_t;intptr_t;uintptr_t;bool;FILE;DIR;time_t;pid_t;uid_t;gid_t;off_t;mode_t;wchar_t;sigset_t;siginfo_t"

fn tokenize(str source, list out_tokens) list {
   "Runs the tokenize operation."
   _h.tokenize_c_like(source, out_tokens, KW, TP, "", "", ".xobeE+-LUlu", "+-*/%=!<>&|^~?", "()[]{};,.:", 47, true, 10, false, true)
}
