;; Keywords: regex regexp re pattern parser text
;; Pure Ny regular-expression engine for parsing-heavy scripts.
module std.core.regex(IGNORECASE, MULTILINE, DOTALL, I, M, S, compile, search, match_start, fullmatch, matches, contains, finditer, findall, sub, split, escape, group, groups, start, end, span)
use std.core
use std.core.str as strmod

def IGNORECASE = 1
def MULTILINE = 2
def DOTALL = 4
def I = IGNORECASE
def M = MULTILINE
def S = DOTALL

fn _lower_byte(int: c): int {
   if(c >= 65 && c <= 90){ return c + 32 }
   c
}

fn _eq_ch(int: a, int: b, int: flags): bool {
   if((flags & IGNORECASE) != 0){ return _lower_byte(a) == _lower_byte(b) }
   a == b
}

fn _is_digit(int: c): bool { c >= 48 && c <= 57 }
fn _is_space(int: c): bool { c == 32 || c == 9 || c == 10 || c == 13 || c == 12 || c == 11 }
fn _is_word(int: c): bool {
   (c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c == 95
}

fn _slice(str: s, int: a, int: b): str {
   if(a < 0){ a = 0 }
   if(b < a){ b = a }
   if(b > s.len){ b = s.len }
   strmod._substr(s, a, b)
}

fn _node(str: t): dict { {"t": t} }

fn _n_lit(int: c): dict {
   mut n = _node("lit")
   n["c"] = c
   n
}

fn _n_class(bool: neg, list: items): dict {
   mut n = _node("class")
   n["neg"] = neg
   n["items"] = items
   n
}

fn _n_anchor(str: k): dict {
   mut n = _node("anchor")
   n["k"] = k
   n
}

fn _n_seq(list: xs): dict {
   if(xs.len == 0){ return _node("empty") }
   if(xs.len == 1){ return xs[0] }
   mut n = _node("seq")
   n["xs"] = xs
   n
}

fn _n_alt(list: xs): dict {
   if(xs.len == 1){ return xs[0] }
   mut n = _node("alt")
   n["xs"] = xs
   n
}

fn _n_rep(dict: child, int: mn, int: mx, bool: greedy): dict {
   mut n = _node("rep")
   n["child"] = child
   n["min"] = mn
   n["max"] = mx
   n["greedy"] = greedy
   n
}

fn _n_cap(int: idx, dict: child, str: name=""): dict {
   mut n = _node("cap")
   n["idx"] = idx
   n["child"] = child
   n["name"] = name
   n
}

fn _n_look(dict: child, bool: neg, bool: behind): dict {
   mut n = _node(behind ? "lookbehind" : "look")
   n["child"] = child
   n["neg"] = neg
   n
}

fn _n_backref(int: idx): dict {
   mut n = _node("backref")
   n["idx"] = idx
   n
}

fn _ctx_group(dict: ctx, str: name=""): int {
   def idx = ctx.get("groups", 0) + 1
   ctx["groups"] = idx
   if(name.len > 0){
      mut names = ctx.get("names", dict(8))
      names[name] = idx
      ctx["names"] = names
   }
   idx
}

fn _parse_number(str: pat, int: pos): list {
   mut v = 0
   mut p = pos
   mut any_digit = false
   while(p < pat.len && _is_digit(load8(pat, p))){
      any_digit = true
      v = v * 10 + (load8(pat, p) - 48)
      p += 1
   }
   [v, p, any_digit]
}

fn _escape_node(str: pat, int: pos): list {
   if(pos >= pat.len){ return [_n_lit(92), pos] }
   def c = load8(pat, pos)
   if(c == 100){ return [_n_class(false, [["cat", "d"]]), pos + 1] }
   if(c == 68){ return [_n_class(true, [["cat", "d"]]), pos + 1] }
   if(c == 119){ return [_n_class(false, [["cat", "w"]]), pos + 1] }
   if(c == 87){ return [_n_class(true, [["cat", "w"]]), pos + 1] }
   if(c == 115){ return [_n_class(false, [["cat", "s"]]), pos + 1] }
   if(c == 83){ return [_n_class(true, [["cat", "s"]]), pos + 1] }
   if(c == 98){ return [_n_anchor("b"), pos + 1] }
   if(c == 66){ return [_n_anchor("B"), pos + 1] }
   if(c == 65){ return [_n_anchor("A"), pos + 1] }
   if(c == 90){ return [_n_anchor("Z"), pos + 1] }
   if(c == 110){ return [_n_lit(10), pos + 1] }
   if(c == 116){ return [_n_lit(9), pos + 1] }
   if(c == 114){ return [_n_lit(13), pos + 1] }
   if(c >= 49 && c <= 57){
      def r = _parse_number(pat, pos)
      return [_n_backref(r[0]), r[1]]
   }
   [_n_lit(c), pos + 1]
}

fn _class_item(str: pat, int: pos): list {
   if(pos >= pat.len){ return [["ch", 0], pos] }
   def c = load8(pat, pos)
   if(c != 92){ return [["ch", c], pos + 1] }
   if(pos + 1 >= pat.len){ return [["ch", 92], pos + 1] }
   def e = load8(pat, pos + 1)
   if(e == 100){ return [["cat", "d"], pos + 2] }
   if(e == 68){ return [["ncat", "d"], pos + 2] }
   if(e == 119){ return [["cat", "w"], pos + 2] }
   if(e == 87){ return [["ncat", "w"], pos + 2] }
   if(e == 115){ return [["cat", "s"], pos + 2] }
   if(e == 83){ return [["ncat", "s"], pos + 2] }
   if(e == 110){ return [["ch", 10], pos + 2] }
   if(e == 116){ return [["ch", 9], pos + 2] }
   if(e == 114){ return [["ch", 13], pos + 2] }
   [["ch", e], pos + 2]
}

fn _parse_class(str: pat, int: pos): list {
   mut p = pos + 1
   mut neg = false
   if(p < pat.len && load8(pat, p) == 94){
      neg = true
      p += 1
   }
   mut items = []
   while(p < pat.len){
      if(load8(pat, p) == 93 && items.len > 0){ return [_n_class(neg, items), p + 1] }
      def a = _class_item(pat, p)
      def first = a[0]
      p = a[1]
      if(first[0] == "ch" && p + 1 < pat.len && load8(pat, p) == 45 && load8(pat, p + 1) != 93){
         def b = _class_item(pat, p + 1)
         def second = b[0]
         if(second[0] == "ch"){
            items = items.append(["range", first[1], second[1]])
            p = b[1]
         } else {
            items = items.append(first)
            items = items.append(["ch", 45])
            items = items.append(second)
            p = b[1]
         }
      } else {
         items = items.append(first)
      }
   }
   panic("unterminated character class")
}

fn _parse_group(dict: ctx, int: pos): list {
   def pat = ctx["pattern"]
   if(pos + 1 < pat.len && load8(pat, pos + 1) == 63){
      def k = pos + 2 < pat.len ? load8(pat, pos + 2) : 0
      if(k == 58){
         def r = _parse_alt(ctx, pos + 3, 41)
         if(r[1] >= pat.len || load8(pat, r[1]) != 41){ panic("unterminated group") }
         return [r[0], r[1] + 1]
      }
      if(k == 61 || k == 33){
         def r = _parse_alt(ctx, pos + 3, 41)
         if(r[1] >= pat.len || load8(pat, r[1]) != 41){ panic("unterminated lookahead") }
         return [_n_look(r[0], k == 33, false), r[1] + 1]
      }
      if(k == 60 && pos + 3 < pat.len && (load8(pat, pos + 3) == 61 || load8(pat, pos + 3) == 33)){
         def neg = load8(pat, pos + 3) == 33
         def r = _parse_alt(ctx, pos + 4, 41)
         if(r[1] >= pat.len || load8(pat, r[1]) != 41){ panic("unterminated lookbehind") }
         return [_n_look(r[0], neg, true), r[1] + 1]
      }
      if(k == 80 && pos + 3 < pat.len && load8(pat, pos + 3) == 60){
         mut p = pos + 4
         while(p < pat.len && load8(pat, p) != 62){ p += 1 }
         if(p >= pat.len){ panic("unterminated named group") }
         def name = _slice(pat, pos + 4, p)
         def idx = _ctx_group(ctx, name)
         def r = _parse_alt(ctx, p + 1, 41)
         if(r[1] >= pat.len || load8(pat, r[1]) != 41){ panic("unterminated named group") }
         return [_n_cap(idx, r[0], name), r[1] + 1]
      }
      if(k == 80 && pos + 3 < pat.len && load8(pat, pos + 3) == 61){
         mut p = pos + 4
         while(p < pat.len && load8(pat, p) != 41){ p += 1 }
         def name = _slice(pat, pos + 4, p)
         def names = ctx.get("names", dict(0))
         return [_n_backref(names.get(name, 0)), p + 1]
      }
      mut p = pos + 2
      mut flags = ctx.get("flags", 0)
      while(p < pat.len && load8(pat, p) != 58 && load8(pat, p) != 41){
         def fc = load8(pat, p)
         if(fc == 105){ flags = flags | IGNORECASE }
         elif(fc == 109){ flags = flags | MULTILINE }
         elif(fc == 115){ flags = flags | DOTALL }
         p += 1
      }
      ctx["flags"] = flags
      if(p < pat.len && load8(pat, p) == 41){ return [_node("empty"), p + 1] }
      if(p < pat.len && load8(pat, p) == 58){
         def r = _parse_alt(ctx, p + 1, 41)
         if(r[1] >= pat.len || load8(pat, r[1]) != 41){ panic("unterminated flags group") }
         return [r[0], r[1] + 1]
      }
   }
   def idx = _ctx_group(ctx)
   def r = _parse_alt(ctx, pos + 1, 41)
   if(r[1] >= pat.len || load8(pat, r[1]) != 41){ panic("unterminated group") }
   [_n_cap(idx, r[0]), r[1] + 1]
}

fn _parse_atom(dict: ctx, int: pos): list {
   def pat = ctx["pattern"]
   if(pos >= pat.len){ return [_node("empty"), pos] }
   def c = load8(pat, pos)
   if(c == 40){ return _parse_group(ctx, pos) }
   if(c == 91){ return _parse_class(pat, pos) }
   if(c == 46){
      mut n = _node("dot")
      return [n, pos + 1]
   }
   if(c == 94){ return [_n_anchor("^"), pos + 1] }
   if(c == 36){ return [_n_anchor("$"), pos + 1] }
   if(c == 92){ return _escape_node(pat, pos + 1) }
   [_n_lit(c), pos + 1]
}

fn _parse_piece(dict: ctx, int: pos): list {
   def a = _parse_atom(ctx, pos)
   mut node = a[0]
   mut p = a[1]
   def pat = ctx["pattern"]
   if(p >= pat.len){ return [node, p] }
   def c = load8(pat, p)
   mut mn = -2
   mut mx = -2
   if(c == 42){ mn = 0 mx = -1 p += 1 }
   elif(c == 43){ mn = 1 mx = -1 p += 1 }
   elif(c == 63){ mn = 0 mx = 1 p += 1 }
   elif(c == 123){
      def r1 = _parse_number(pat, p + 1)
      if(!r1[2]){ return [node, p] }
      mn = r1[0]
      mx = mn
      p = r1[1]
      if(p < pat.len && load8(pat, p) == 44){
         p += 1
         def r2 = _parse_number(pat, p)
         if(r2[2]){ mx = r2[0] p = r2[1] } else { mx = -1 }
      }
      if(p >= pat.len || load8(pat, p) != 125){ panic("unterminated quantifier") }
      p += 1
   }
   if(mn == -2){ return [node, p] }
   mut greedy = true
   if(p < pat.len && load8(pat, p) == 63){
      greedy = false
      p += 1
   }
   [_n_rep(node, mn, mx, greedy), p]
}

fn _parse_seq(dict: ctx, int: pos, int: end_ch): list {
   def pat = ctx["pattern"]
   mut p = pos
   mut xs = []
   while(p < pat.len){
      def c = load8(pat, p)
      if(c == 124 || (end_ch != 0 && c == end_ch)){ break }
      def r = _parse_piece(ctx, p)
      xs = xs.append(r[0])
      p = r[1]
   }
   [_n_seq(xs), p]
}

fn _parse_alt(dict: ctx, int: pos, int: end_ch): list {
   mut branches = []
   def first = _parse_seq(ctx, pos, end_ch)
   branches = branches.append(first[0])
   mut p = first[1]
   def pat = ctx["pattern"]
   while(p < pat.len && load8(pat, p) == 124){
      def r = _parse_seq(ctx, p + 1, end_ch)
      branches = branches.append(r[0])
      p = r[1]
   }
   [_n_alt(branches), p]
}

fn compile(any: pattern, int: flags=0): dict {
   "Compiles a regex pattern into a reusable pure-Ny regex object."
   if(is_dict(pattern) && pattern.contains("ast")){ return pattern }
   if(!is_str(pattern)){ pattern = to_str(pattern) }
   mut ctx = {"pattern": pattern, "flags": flags, "groups": 0, "names": dict(8)}
   def r = _parse_alt(ctx, 0, 0)
   if(r[1] != pattern.len){ panic("unexpected ')' in regex pattern") }
   {"pattern": pattern, "flags": ctx.get("flags", flags), "ast": r[0], "groups": ctx.get("groups", 0), "names": ctx.get("names", dict(0))}
}

fn _empty_caps(int: groups): list {
   mut out = []
   mut i = 0
   while(i <= groups){
      out = out.append([-1, -1])
      i += 1
   }
   out
}

fn _copy_caps(list: caps): list {
   mut out = []
   mut i = 0
   while(i < caps.len){
      def p = caps.get(i, [-1, -1])
      out = out.append([p.get(0, -1), p.get(1, -1)])
      i += 1
   }
   out
}

fn _caps_set(list: caps, int: idx, int: a, int: b): list {
   mut out = _copy_caps(caps)
   while(out.len <= idx){ out = out.append([-1, -1]) }
   out[idx] = [a, b]
   out
}

fn _state(int: pos, list: caps): list { [pos, caps] }
fn _state_pos(list: st): int { st.get(0, 0) }
fn _state_caps(list: st): list { st.get(1, []) }

fn _class_cat_match(str: cat, int: c): bool {
   if(cat == "d"){ return _is_digit(c) }
   if(cat == "w"){ return _is_word(c) }
   if(cat == "s"){ return _is_space(c) }
   false
}

fn _class_item_match(list: item, int: c, int: flags): bool {
   def k = item.get(0, "")
   if(k == "ch"){ return _eq_ch(c, item.get(1, 0), flags) }
   if(k == "range"){
      mut a = item.get(1, 0)
      mut b = item.get(2, 0)
      mut cc = c
      if((flags & IGNORECASE) != 0){
         a = _lower_byte(a)
         b = _lower_byte(b)
         cc = _lower_byte(cc)
      }
      return cc >= a && cc <= b
   }
   if(k == "cat"){ return _class_cat_match(item.get(1, ""), c) }
   if(k == "ncat"){ return !_class_cat_match(item.get(1, ""), c) }
   false
}

fn _class_match(dict: node, int: c, int: flags): bool {
   def items = node.get("items", [])
   mut ok = false
   mut i = 0
   while(i < items.len){
      if(_class_item_match(items[i], c, flags)){ ok = true break }
      i += 1
   }
   node.get("neg", false) ? !ok : ok
}

fn _word_boundary(str: text, int: pos): bool {
   def left = pos > 0 && _is_word(load8(text, pos - 1))
   def right = pos < text.len && _is_word(load8(text, pos))
   left != right
}

fn _repeat_collect(dict: child, str: text, list: st, int: count, int: mn, int: mx, bool: greedy, int: flags, int: depth): list {
   if(depth > 10000){ return [] }
   mut out = []
   if(count >= mn && !greedy){ out = out.append(st) }
   if(mx < 0 || count < mx){
      def nexts = _match_node(child, text, _state_pos(st), _state_caps(st), flags, depth + 1)
      mut i = 0
      while(i < nexts.len){
         def ns = nexts[i]
         if(_state_pos(ns) != _state_pos(st)){
            def more = _repeat_collect(child, text, ns, count + 1, mn, mx, greedy, flags, depth + 1)
            mut j = 0
            while(j < more.len){ out = out.append(more[j]) j += 1 }
         }
         i += 1
      }
   }
   if(count >= mn && greedy){ out = out.append(st) }
   out
}

fn _match_seq(list: xs, str: text, int: pos, list: caps, int: flags, int: depth): list {
   mut states = [_state(pos, caps)]
   mut i = 0
   while(i < xs.len){
      mut next = []
      mut j = 0
      while(j < states.len){
         def st = states[j]
         def rs = _match_node(xs[i], text, _state_pos(st), _state_caps(st), flags, depth + 1)
         mut k = 0
         while(k < rs.len){ next = next.append(rs[k]) k += 1 }
         j += 1
      }
      states = next
      if(states.len == 0){ return [] }
      i += 1
   }
   states
}

fn _lookbehind_ok(dict: child, str: text, int: pos, list: caps, int: flags, int: depth): bool {
   mut start_pos = 0
   while(start_pos <= pos){
      def rs = _match_node(child, text, start_pos, caps, flags, depth + 1)
      mut i = 0
      while(i < rs.len){
         if(_state_pos(rs[i]) == pos){ return true }
         i += 1
      }
      start_pos += 1
   }
   false
}

fn _backref_match(dict: node, str: text, int: pos, list: caps, int: flags): list {
   def idx = node.get("idx", 0)
   if(idx <= 0 || idx >= caps.len){ return [] }
   def sp = caps.get(idx, [-1, -1])
   def a = sp.get(0, -1)
   def b = sp.get(1, -1)
   if(a < 0 || b < a){ return [] }
   def n = b - a
   if(pos + n > text.len){ return [] }
   mut i = 0
   while(i < n){
      if(!_eq_ch(load8(text, a + i), load8(text, pos + i), flags)){ return [] }
      i += 1
   }
   [_state(pos + n, caps)]
}

fn _match_node(dict: node, str: text, int: pos, list: caps, int: flags, int: depth): list {
   if(depth > 10000){ return [] }
   def t = node.get("t", "empty")
   if(t == "empty"){ return [_state(pos, caps)] }
   if(t == "lit"){
      if(pos < text.len && _eq_ch(load8(text, pos), node.get("c", 0), flags)){ return [_state(pos + 1, caps)] }
      return []
   }
   if(t == "dot"){
      if(pos < text.len && (((flags & DOTALL) != 0) || load8(text, pos) != 10)){ return [_state(pos + 1, caps)] }
      return []
   }
   if(t == "class"){
      if(pos < text.len && _class_match(node, load8(text, pos), flags)){ return [_state(pos + 1, caps)] }
      return []
   }
   if(t == "anchor"){
      def k = node.get("k", "")
      if(k == "^"){ return (pos == 0 || ((flags & MULTILINE) != 0 && pos > 0 && load8(text, pos - 1) == 10)) ? [_state(pos, caps)] : [] }
      if(k == "$"){ return (pos == text.len || (pos == text.len - 1 && load8(text, pos) == 10) || (((flags & MULTILINE) != 0) && pos < text.len && load8(text, pos) == 10)) ? [_state(pos, caps)] : [] }
      if(k == "A"){ return pos == 0 ? [_state(pos, caps)] : [] }
      if(k == "Z"){ return (pos == text.len || (pos == text.len - 1 && load8(text, pos) == 10)) ? [_state(pos, caps)] : [] }
      if(k == "b"){ return _word_boundary(text, pos) ? [_state(pos, caps)] : [] }
      if(k == "B"){ return !_word_boundary(text, pos) ? [_state(pos, caps)] : [] }
      return []
   }
   if(t == "seq"){ return _match_seq(node.get("xs", []), text, pos, caps, flags, depth + 1) }
   if(t == "alt"){
      mut out = []
      def xs = node.get("xs", [])
      mut i = 0
      while(i < xs.len){
         def rs = _match_node(xs[i], text, pos, caps, flags, depth + 1)
         mut j = 0
         while(j < rs.len){ out = out.append(rs[j]) j += 1 }
         i += 1
      }
      return out
   }
   if(t == "cap"){
      def idx = node.get("idx", 0)
      def rs = _match_node(node["child"], text, pos, caps, flags, depth + 1)
      mut out = []
      mut i = 0
      while(i < rs.len){
         def st = rs[i]
         out = out.append(_state(_state_pos(st), _caps_set(_state_caps(st), idx, pos, _state_pos(st))))
         i += 1
      }
      return out
   }
   if(t == "rep"){
      return _repeat_collect(node["child"], text, _state(pos, caps), 0, node.get("min", 0), node.get("max", -1), node.get("greedy", true), flags, depth + 1)
   }
   if(t == "look"){
      def ok = _match_node(node["child"], text, pos, caps, flags, depth + 1).len > 0
      return (node.get("neg", false) ? !ok : ok) ? [_state(pos, caps)] : []
   }
   if(t == "lookbehind"){
      def ok = _lookbehind_ok(node["child"], text, pos, caps, flags, depth + 1)
      return (node.get("neg", false) ? !ok : ok) ? [_state(pos, caps)] : []
   }
   if(t == "backref"){ return _backref_match(node, text, pos, caps, flags) }
   []
}

fn _make_match(dict: rx, str: text, int: start_pos, list: st): dict {
   def caps = _caps_set(_state_caps(st), 0, start_pos, _state_pos(st))
   {"re": rx, "text": text, "start": start_pos, "end": _state_pos(st), "caps": caps}
}

fn _run_at(dict: rx, str: text, int: pos): any {
   def caps = _empty_caps(rx.get("groups", 0))
   def rs = _match_node(rx["ast"], text, pos, caps, rx.get("flags", 0), 0)
   if(rs.len == 0){ return nil }
   _make_match(rx, text, pos, rs[0])
}

fn match_start(any: pattern, any: text, int: flags=0): any {
   "Matches `pattern` at the start of `text`, returning a match object or nil."
   if(!is_str(text)){ text = to_str(text) }
   _run_at(compile(pattern, flags), text, 0)
}

fn fullmatch(any: pattern, any: text, int: flags=0): any {
   "Matches the entire text."
   if(!is_str(text)){ text = to_str(text) }
   def rx = compile(pattern, flags)
   def m = _run_at(rx, text, 0)
   if(m && m.get("end", -1) == text.len){ return m }
   nil
}

fn search(any: pattern, any: text, int: flags=0): any {
   "Searches `text` for `pattern`, returning a match object or nil."
   if(!is_str(text)){ text = to_str(text) }
   def rx = compile(pattern, flags)
   mut pos = 0
   while(pos <= text.len){
      def m = _run_at(rx, text, pos)
      if(m){ return m }
      pos += 1
   }
   nil
}

fn matches(any: pattern, any: text, int: flags=0): bool { fullmatch(pattern, text, flags) != nil }
fn contains(any: text, any: pattern, int: flags=0): bool { search(pattern, text, flags) != nil }

fn start(dict: m, any: idx=0): int {
   def g = group(m, idx)
   if(g == nil){ return -1 }
   def gi = is_str(idx) ? m["re"].get("names", dict(0)).get(idx, -1) : idx
   m.get("caps", []).get(gi, [-1, -1]).get(0, -1)
}

fn end(dict: m, any: idx=0): int {
   def g = group(m, idx)
   if(g == nil){ return -1 }
   def gi = is_str(idx) ? m["re"].get("names", dict(0)).get(idx, -1) : idx
   m.get("caps", []).get(gi, [-1, -1]).get(1, -1)
}

fn span(dict: m, any: idx=0): list { [start(m, idx), end(m, idx)] }

fn group(dict: m, any: idx=0): any {
   mut gi = idx
   if(is_str(idx)){ gi = m["re"].get("names", dict(0)).get(idx, -1) }
   if(!is_int(gi) || gi < 0 || gi >= m.get("caps", []).len){ return nil }
   def p = m["caps"].get(gi, [-1, -1])
   def a = p.get(0, -1)
   def b = p.get(1, -1)
   if(a < 0 || b < a){ return nil }
   _slice(m["text"], a, b)
}

fn groups(dict: m): list {
   mut out = []
   def n = m["re"].get("groups", 0)
   mut i = 1
   while(i <= n){
      out = out.append(group(m, i))
      i += 1
   }
   out
}

fn finditer(any: pattern, any: text, int: flags=0): list {
   if(!is_str(text)){ text = to_str(text) }
   def rx = compile(pattern, flags)
   mut out = []
   mut pos = 0
   while(pos <= text.len){
      mut found = nil
      mut scan = pos
      while(scan <= text.len && !found){
         found = _run_at(rx, text, scan)
         scan += 1
      }
      if(!found){ break }
      out = out.append(found)
      def a = found.get("start", pos)
      def b = found.get("end", a)
      pos = b > a ? b : a + 1
   }
   out
}

fn findall(any: pattern, any: text, int: flags=0): list {
   def ms = finditer(pattern, text, flags)
   mut out = []
   mut i = 0
   while(i < ms.len){
      def m = ms[i]
      def n = m["re"].get("groups", 0)
      if(n == 0){ out = out.append(group(m, 0)) }
      elif(n == 1){ out = out.append(group(m, 1)) }
      else { out = out.append(groups(m)) }
      i += 1
   }
   out
}

fn _expand_repl(any: repl, dict: m): str {
   if(!is_str(repl)){ repl = to_str(repl) }
   mut out = ""
   mut i = 0
   while(i < repl.len){
      def c = load8(repl, i)
      if(c == 92 && i + 1 < repl.len){
         def n = load8(repl, i + 1)
         if(_is_digit(n)){
            def r = _parse_number(repl, i + 1)
            def g = group(m, r[0])
            if(g != nil){ out = out + g }
            i = r[1]
         } elif(n == 103 && i + 2 < repl.len && load8(repl, i + 2) == 60){
            mut p = i + 3
            while(p < repl.len && load8(repl, p) != 62){ p += 1 }
            def name = _slice(repl, i + 3, p)
            def g = group(m, name)
            if(g != nil){ out = out + g }
            i = p + 1
         } else {
            out = out + chr(n)
            i += 2
         }
      } else {
         out = out + chr(c)
         i += 1
      }
   }
   out
}

fn sub(any: pattern, any: repl, any: text, int: count=0, int: flags=0): str {
   "Replaces regex matches. Replacement supports \\1 and \\g<name> references."
   if(!is_str(text)){ text = to_str(text) }
   def rx = compile(pattern, flags)
   mut out = ""
   mut pos = 0
   mut done = 0
   while(pos <= text.len){
      if(count > 0 && done >= count){ break }
      mut m = nil
      mut scan = pos
      while(scan <= text.len && !m){
         m = _run_at(rx, text, scan)
         scan += 1
      }
      if(!m){ break }
      out = out + _slice(text, pos, m["start"]) + _expand_repl(repl, m)
      done += 1
      if(m["end"] == m["start"]){
         if(m["end"] < text.len){ out = out + _slice(text, m["end"], m["end"] + 1) }
         pos = m["end"] + 1
      } else {
         pos = m["end"]
      }
   }
   out + _slice(text, pos, text.len)
}

fn split(any: pattern, any: text, int: maxsplit=0, int: flags=0): list {
   "Splits text by regex matches. Capturing groups are included like Python re.split."
   if(!is_str(text)){ text = to_str(text) }
   def rx = compile(pattern, flags)
   mut out = []
   mut pos = 0
   mut done = 0
   while(pos <= text.len){
      if(maxsplit > 0 && done >= maxsplit){ break }
      mut m = nil
      mut scan = pos
      while(scan <= text.len && !m){
         m = _run_at(rx, text, scan)
         scan += 1
      }
      if(!m){ break }
      out = out.append(_slice(text, pos, m["start"]))
      def gs = groups(m)
      mut i = 0
      while(i < gs.len){ out = out.append(gs[i]) i += 1 }
      done += 1
      pos = m["end"] > m["start"] ? m["end"] : m["end"] + 1
   }
   out = out.append(_slice(text, pos, text.len))
   out
}

fn escape(any: s): str {
   "Escapes a literal string so it can be used as a regex pattern."
   if(!is_str(s)){ s = to_str(s) }
   mut out = ""
   mut i = 0
   while(i < s.len){
      def c = load8(s, i)
      if(_is_word(c)){ out = out + chr(c) }
      else { out = out + "\\" + chr(c) }
      i += 1
   }
   out
}
