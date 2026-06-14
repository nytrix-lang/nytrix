;; Keywords: syntax html markup parse highlight
;; HTML syntax highlighter
;; References:
;; - std.parse.syntax
;; - std.parse.syntax.helpers
module std.parse.syntax.html(tokenize)
use std.core
use std.core.str as str
use std.parse.syntax.helpers as _h

def TAGS = "html;head;body;title;meta;link;style;script;div;span;p;a;img;ul;ol;li;table;tr;td;th;thead;tbody;tfoot;form;input;button;select;option;textarea;label;h1;h2;h3;h4;h5;h6;header;footer;nav;main;section;article;aside;details;summary;dialog;canvas;svg;video;audio;source;track;iframe;object;embed;map;area;br;hr;pre;code;blockquote;q;em;strong;small;mark;del;ins;sub;sup;abbr;address;cite;dfn;time;var;samp;kbd;figure;figcaption"
def ATTRS = "id;class;style;title;lang;dir;hidden;tabindex;accesskey;contenteditable;draggable;spellcheck;translate;data;src;href;alt;name;value;type;placeholder;required;disabled;readonly;checked;selected;multiple;maxlength;min;max;step;pattern;autocomplete;autofocus;novalidate;formaction;formmethod;formenctype;formtarget;formnovalidate;list;width;height;poster;preload;controls;loop;muted;autoplay;colspan;rowspan;scope;headers;colgroup;col;caption;datetime;cite;hreflang;rel;rev;target;media;sizes;sandbox;srcdoc;allowfullscreen;frameborder;scrolling;marginwidth;marginheight;align;valign;border;cellpadding;cellspacing;bgcolor;background;face;size;color"

fn tokenize(str source, list out_tokens) list {
   "Runs the tokenize operation."
   def src_len = source.len
   mut i = 0
   while i < src_len {
      def idx = i + 0
      i = idx
      def ch = load8(source, i)
      if _h.is_space_ch(ch) {
         def j = _h.scan_space(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 14, i, j - i)
         i = j
      } elif ch == 60 && i + 3 < src_len && load8(source, i+1) == 33 && load8(source, i+2) == 45 && load8(source, i+3) == 45 {
         mut j = i + 4
         while j + 2 < src_len { if load8(source, j) == 45 && load8(source, j + 1) == 45 && load8(source, j + 2) == 62 { j += 3 break } j += 1 }
         out_tokens = _h.add_tok(out_tokens, 4, i, j - i)
         i = j
      } elif ch == 60 {
         mut j = i + 1
         if j < src_len && load8(source, j) == 47 { j += 1 }
         while j < src_len && _h.is_alnum_ch(load8(source, j)) { j += 1 }
         def word = str.str_slice(source, i + 1, j)
         if _h.in_list(word, TAGS) { out_tokens = _h.add_tok(out_tokens, 18, i, j - i) }
         else { out_tokens = _h.add_tok(out_tokens, 18, i, j - i) }
         i = j
      } elif ch == 62 {
         out_tokens = _h.add_tok(out_tokens, 7, i, 1)
         i += 1
      } elif ch == 34 || ch == 39 {
         def j = _h.scan_quoted(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 2, i, j - i)
         i = j
      } elif _h.is_alpha_ch(ch) || ch == 95 || ch == 45 {
         def j = _h.scan_ident_extra(source, i, src_len, "-")
         def word = str.str_slice(source, i, j)
         if _h.in_list(word, ATTRS) { out_tokens = _h.add_tok(out_tokens, 19, i, j - i) }
         else { out_tokens = _h.add_tok(out_tokens, 19, i, j - i) }
         i = j
      } elif ch == 61 {
         out_tokens = _h.add_tok(out_tokens, 6, i, 1)
         i += 1
      } elif ch == 47 {
         out_tokens = _h.add_tok(out_tokens, 7, i, 1)
         i += 1
      } else {
         out_tokens = _h.add_tok(out_tokens, 14, i, 1)
         i += 1
      }
   }
   out_tokens
}
