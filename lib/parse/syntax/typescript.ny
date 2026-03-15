;; Keywords: syntax typescript ts
;; TypeScript syntax highlighter
module std.parse.syntax.typescript(tokenize)
use std.parse.syntax.helpers as _h

def KW = "break;case;catch;continue;debugger;default;delete;do;else;finally;for;function;if;in;instanceof;new;return;switch;this;throw;try;typeof;var;void;while;with;class;const;enum;export;extends;import;super;implements;interface;let;package;private;protected;public;static;yield;await;async;from;of;true;false;null;undefined;type;namespace;declare;abstract;readonly;as;is;keyof;infer"
def TP = "Array;Boolean;Date;Error;Function;JSON;Math;Number;Object;RegExp;String;Symbol;Map;Set;WeakMap;WeakSet;Promise;Proxy;Reflect;Int8Array;Uint8Array;Float32Array;Float64Array"
def FN = "console;log;warn;error;info;setTimeout;setInterval;clearTimeout;clearInterval;parseInt;parseFloat;isNaN;isFinite;eval;fetch;require"

fn tokenize(str: source, list: out_tokens): list {
   _h.tokenize_c_like(source, out_tokens, KW, TP, FN, "$", ".xobeE+-n", "+-*/%=!<>&|^~?", "()[]{};,.:@#", 47, true, -1, true, false)
}
