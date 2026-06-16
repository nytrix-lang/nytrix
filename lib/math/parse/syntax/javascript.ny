;; Keywords: syntax javascript js parse highlight
;; JavaScript syntax highlighter
;; References:
;; - std.math.parse.syntax
;; - std.math.parse.syntax.helpers
module std.math.parse.syntax.javascript(tokenize)
use std.math.parse.syntax.helpers as _h

def KW = "break;case;catch;continue;debugger;default;delete;do;else;finally;for;function;if;in;instanceof;new;return;switch;this;throw;try;typeof;var;void;while;with;class;const;enum;export;extends;import;super;implements;interface;let;package;private;protected;public;static;yield;await;async;from;of;true;false;null;undefined"
def TP = "Array;Boolean;Date;Error;Function;JSON;Math;Number;Object;RegExp;String;Symbol;Map;Set;WeakMap;WeakSet;Promise;Proxy;Reflect;Int8Array;Uint8Array;Float32Array;Float64Array"
def FN = "console;log;warn;error;info;setTimeout;setInterval;clearTimeout;clearInterval;parseInt;parseFloat;isNaN;isFinite;eval;fetch;require"

fn tokenize(str source, list out_tokens) list {
   "Runs the tokenize operation."
   _h.tokenize_c_like(source, out_tokens, KW, TP, FN, "$", ".xobeE+-n", "+-*/%=!<>&|^~?", "()[]{};,.:@#", 47, true, -1, true, false)
}
