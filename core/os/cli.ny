;; Keywords: cli args options parser command-line os
;; Declarative command-line parsing for Nytrix programs.
;; References:
;; - core.os.args
module core.os.cli(app, flag, option, optional_option, positional, rest, family, subcommand, add, parse, usage, completions, format_error)
use core
use core.str as str
use core.os.args as process_args
use core.os (file_read)

fn app(str name, str description="", str version="") dict {
   "Creates a CLI specification. Add descriptors with `add(spec, descriptor)`."
   {"name": name, "description": description, "version":version, "options": [], "positionals": [], "families": [], "subcommands": dict(4)}
}

fn flag(str long, str short="", str description="", bool default=false, bool repeatable=false, str env="", any conflicts=[]) dict {
   "Declares a boolean option. Long names omit the leading `--`."
   {"kind":"flag", "long":long, "short":short, "description":description, "default":default, "repeatable":repeatable, "env":env, "conflicts":conflicts}
}

fn option(str long, str short="", str description="", str value_type="str", any default=nil, bool required=false, bool repeatable=false, str env="", any conflicts=[]) dict {
   "Declares a typed option. Supported types are str, int, float, path, and list."
   {"kind":"option", "long":long, "short":short, "description":description, "type":value_type, "default":default, "required":required, "repeatable":repeatable, "env":env, "conflicts":conflicts}
}

fn optional_option(str long, str short="", str description="", str value_type="str", any default=nil, any implicit_value=nil) dict {
   "Declares a value option whose long form also accepts a bare spelling with an implicit value."
   mut out = option(long, short, description, value_type, default)
   out["optional_value"] = true
   out["implicit_value"] = implicit_value
   out
}

fn positional(str name, str description="", str value_type="str", bool required=true) dict {
   "Declares a positional operand."
   {"kind":"positional", "name":name, "description":description, "type":value_type, "required":required}
}

fn rest(str name="args", str description="Program arguments after `--`.") dict {
   "Declares a positional tail which receives all remaining arguments."
   {"kind":"rest", "name":name, "description":description}
}

fn family(str long, str short, dict items, str description="") dict {
   "Declares a short-option family such as `-Dnv` for --dump=nyr,vm."
   {"kind":"family", "long":long, "short":short, "items":items, "description":description}
}

fn subcommand(str name, dict spec, str description="") dict {
   "Declares a named subcommand with its own CLI specification."
   {"kind":"subcommand", "name":name, "spec":spec, "description":description}
}

fn add(dict spec, dict descriptor) dict {
   "Returns a specification containing the supplied option, positional, family, or subcommand."
   def kind = to_str(descriptor.get("kind", ""))
   if kind == "positional" || kind == "rest" {
      spec["positionals"] = spec.get("positionals", []).append(descriptor)
   } elif kind == "subcommand" {
      mut commands = spec.get("subcommands", dict(4))
      commands[to_str(descriptor.get("name", ""))] = descriptor
      spec["subcommands"] = commands
   } elif kind == "family" {
      spec["families"] = spec.get("families", []).append(descriptor)
   } else {
      spec["options"] = spec.get("options", []).append(descriptor)
   }
   spec
}

fn _error(str name, str message) dict { {"ok":false, "program":name, "error":message} }

fn format_error(dict result) str {
   "Formats a parser error using the conventional `program: message` form."
   to_str(result.get("program", "ny")) + ": " + to_str(result.get("error", "invalid command line"))
}

fn _file_tokens(str text) dict {
   mut words = []
   for token0 in str.split_words(text) {
      def token = str.cstr_to_str(token0)
      if token.len > 0 && str.byte_at(token, 0) == 35 { break }
      words = words.append(token)
   }
   {"ok":true, "words":words}
}

fn _expand_response(list input, int depth=0) dict {
   if depth > 8 { return _error("ny", "response file nesting exceeds 8") }
   mut out = []
   mut boundary = input.len
   mut scan = 0
   while scan < input.len {
      if _is_separator(str.cstr_to_str(input.get(scan, ""))) { boundary = scan break }
      scan += 1
   }
   mut i = 0
   while i < input.len {
      def tok = str.cstr_to_str(input.get(i, ""))
      if i < boundary && tok.len > 1 && str.byte_at(tok, 0) == 64 {
         def loaded = file_read(str.str_slice(tok, 1, tok.len))
         if is_err(loaded) { return _error("ny", "cannot read response file '" + str.str_slice(tok, 1, tok.len) + "'") }
         def parsed = _file_tokens(unwrap(loaded))
         if !bool(parsed.get("ok", false)) { return parsed }
         def nested = _expand_response(parsed.get("words", []), depth + 1)
         if !bool(nested.get("ok", false)) { return nested }
         for word in nested.get("words", []) { out = out.append(str.cstr_to_str(word)) }
      } else { out = out.append(tok) }
      i += 1
   }
   {"ok":true, "words":out}
}

fn _convert(str raw, str typ, str option_name) dict {
   if typ == "str" || typ == "path" { return {"ok":true, "value":raw} }
   if typ == "list" {
      mut items = []
      for item0 in str.split(raw, ",") {
         def item = str.strip(str.cstr_to_str(item0))
         if item.len == 0 { return _error("", "invalid " + option_name + " '" + raw + "' (expected comma-separated values)") }
         items = items.append(item)
      }
      return {"ok":true, "value":items}
   }
   if typ == "int" {
      if raw.len == 0 { return _error("", "invalid " + option_name + " '" + raw + "' (expected int)") }
      mut i = (str.byte_at(raw, 0) == 43 || str.byte_at(raw, 0) == 45) ? 1 : 0
      if i == raw.len { return _error("", "invalid " + option_name + " '" + raw + "' (expected int)") }
      while i < raw.len { def c = str.byte_at(raw, i) if !str.ascii_is_digit(c) { return _error("", "invalid " + option_name + " '" + raw + "' (expected int)") } i += 1 }
      return {"ok":true, "value":str.atoi(raw)}
   }
   if typ == "float" {
      if raw.len == 0 { return _error("", "invalid " + option_name + " '" + raw + "' (expected float)") }
      mut digits = 0
      mut dots = 0
      mut i = (str.byte_at(raw, 0) == 43 || str.byte_at(raw, 0) == 45) ? 1 : 0
      while i < raw.len {
         def c = str.byte_at(raw, i)
         if str.ascii_is_digit(c) { digits += 1 } elif c == 46 { dots += 1 } else { return _error("", "invalid " + option_name + " '" + raw + "' (expected float)") }
         i += 1
      }
      if digits == 0 || dots > 1 { return _error("", "invalid " + option_name + " '" + raw + "' (expected float)") }
      return {"ok":true, "value":str.atof(raw)}
   }
   _error("", "unsupported value type '" + typ + "'")
}

fn _looks_like_option(str token) bool {
   token.len > 1 && str.byte_at(token, 0) == 45
}

fn _append_repeatable(any previous, any value) list {
   mut out = []
   if is_list(previous) {
      mut i = 0
      while i < previous.len {
         out = out.append(previous.get(i))
         i += 1
      }
   }
   out.append(value)
}

fn _is_separator(str token) bool {
   token.len == 2 && str.byte_at(token, 0) == 45 && str.byte_at(token, 1) == 45
}

fn _long_index(list options, str name) int {
   mut i = 0
   while i < options.len { if to_str(options.get(i).get("long", "")) == name { return i } i += 1 }
   -1
}

fn _short_index(list options, str short) int {
   mut i = 0
   while i < options.len { if to_str(options.get(i).get("short", "")) == short { return i } i += 1 }
   -1
}

fn _nearest(str input, list candidates) str {
   if input.len < 3 { return "" }
   for candidate0 in candidates {
      def candidate = str.cstr_to_str(candidate0)
      if candidate.len >= 3 {
         if str.byte_at(input, 0) == str.byte_at(candidate, 0) {
            if str.byte_at(input, 1) == str.byte_at(candidate, 1) {
               if str.byte_at(input, 2) == str.byte_at(candidate, 2) { return candidate }
            }
         }
      }
   }
   ""
}

fn usage(dict spec) str {
   "Builds concise usage text from a CLI specification."
   def name = to_str(spec.get("name", "app"))
   mut out = "Usage: " + name + " [options]"
   for p in spec.get("positionals", []) {
      def label = to_str(p.get("name", "arg"))
      out = out + (bool(p.get("required", false)) ? " <" + label + ">" : " [" + label + "]")
   }
   out = out + "\n" + to_str(spec.get("description", ""))
   for o in spec.get("options", []) {
      out = out + "\n  --" + to_str(o.get("long", ""))
      def short = to_str(o.get("short", ""))
      if short.len > 0 { out = out + ", -" + short }
      if to_str(o.get("kind", "")) == "option" { out = out + " VALUE" }
      out = out + "  " + to_str(o.get("description", ""))
   }
   out + "\n  -h, --help  Show this help"
}

fn completions(dict spec, str shell) str {
   "Generates basic shell completion candidates for bash, zsh, or fish."
   mut names = []
   for o in spec.get("options", []) { names = names.append("--" + to_str(o.get("long", ""))) }
   for f in spec.get("families", []) {
      def family_short = to_str(f.get("short", ""))
      def items = f.get("items", dict(0))
      for item in items.keys() {
         names = names.append("-" + family_short + to_str(item) + "  " + to_str(items.get(item, "")))
      }
   }
   if shell == "bash" { return "complete -W '" + str.join(names, " ") + "' " + to_str(spec.get("name", "app")) }
   if shell == "zsh" { return "#compdef " + to_str(spec.get("name", "app")) + "\n_arguments '*: :(" + str.join(names, " ") + ")'" }
   if shell == "fish" {
      mut out = ""
      for name in names { out = out + "complete -c " + to_str(spec.get("name", "app")) + " -f -a '" + to_str(name) + "'\n" }
      return out
   }
   ""
}

fn _family_option(list families, str letter) int {
   mut i = 0
   while i < families.len { if to_str(families.get(i).get("short", "")) == letter { return i } i += 1 }
   -1
}

fn parse(dict spec, list argv=process_args.args()) dict {
   "Parses argv against a CLI specification and returns {ok, values, positionals, command}; errors are returned as {ok:false, program, error}."
   def name = to_str(spec.get("name", "app"))
   if !is_list(argv) { return _error(name, "invalid argv") }
   mut input = []
   mut start = argv.len > 0 ? 1 : 0
   while start < argv.len { input = input.append(str.cstr_to_str(argv.get(start, ""))) start += 1 }
   def expanded = _expand_response(input)
   if !bool(expanded.get("ok", false)) { mut err = expanded err["program"] = name return err }
   input = expanded.get("words", [])
   def options = spec.get("options", [])
   def families = spec.get("families", [])
   mut values = dict(16)
   mut env_satisfied = dict(16)
   mut errors = []
   for o in options {
      def long = to_str(o.get("long", ""))
      def kind = to_str(o.get("kind", ""))
      def env_name = to_str(o.get("env", ""))
      mut initial = o.get("default", kind == "flag" ? false : nil)
      if bool(o.get("repeatable", false)) { initial = nil }
      if env_name.len > 0 {
         def e = __env_get(env_name)
         if e != 0 {
            def env_value = str.cstr_to_str(e)
            if kind == "flag" {
               def lower = str.lower(env_value)
               if lower == "true" || lower == "1" || lower == "yes" { initial = true }
               elif lower == "false" || lower == "0" || lower == "no" { initial = false }
               else { return _error(name, "invalid environment value for --" + long + " (expected true|false)") }
            } else {
               def converted = _convert(env_value, to_str(o.get("type", "str")), "--" + long)
               if !bool(converted.get("ok", false)) { return _error(name, to_str(converted.get("error", "invalid environment value"))) }
               initial = converted.get("value")
            }
            env_satisfied[long] = true
         }
      }
      values[long] = initial
   }
   mut seen = dict(16)
   mut positional_values = []
   mut boundary = input.len
   mut scan = 0
   while scan < input.len {
      if _is_separator(str.cstr_to_str(input.get(scan, ""))) { boundary = scan break }
      scan += 1
   }
   mut i = 0
   while i < input.len {
      def tok = str.cstr_to_str(input.get(i, ""))
      if i == boundary {
         i += 1
         while i < input.len {
            positional_values = positional_values.append(str.cstr_to_str(input.get(i, "")))
            i += 1
         }
         break
      }
      if i < boundary && (tok == "--help" || tok == "-h") { return {"ok":true, "help":true, "text":usage(spec), "values":values, "provided":seen, "positionals":positional_values, "command":""} }
      if i < boundary && tok == "--version" {
         return {"ok":true, "help":true, "text":to_str(spec.get("version", "version unavailable")), "values":values, "provided":seen, "positionals":positional_values, "command":""}
      }
      if i < boundary && tok == "--completions" {
         if i + 1 >= boundary { return _error(name, "--completions requires bash, zsh, or fish") }
         def shell = to_str(input.get(i + 1, ""))
         def script = completions(spec, shell)
         if script.len == 0 { return _error(name, "invalid --completions '" + shell + "' (expected bash|zsh|fish)") }
         return {"ok":true, "help":true, "text":script, "values":values, "provided":seen, "positionals":positional_values, "command":""}
      }
      if i < boundary && tok == "--print-args" {
         return {"ok":true, "help":true, "text":repr(input.slice(0, input.len)), "values":values, "provided":seen, "positionals":positional_values, "command":""}
      }
      if i < boundary && str.startswith(tok, "--") {
         def eq = str.find(tok, "=")
         def key_raw = eq >= 0 ? str.str_slice(tok, 2, eq) : str.str_slice(tok, 2, tok.len)
         def negated = str.startswith(key_raw, "no-")
         def key = negated ? str.str_slice(key_raw, 3, key_raw.len) : key_raw
         def idx = _long_index(options, key)
         if idx < 0 { mut names = [] for o in options { names = names.append("--" + to_str(o.get("long", ""))) } def near = _nearest("--" + key, names) return _error(name, "unknown option '--" + key_raw + "'" + (near.len > 0 ? " (did you mean '" + near + "'?)" : "")) }
         def o = options.get(idx)
         def kind = to_str(o.get("kind", ""))
         if kind == "flag" {
            if eq >= 0 { return _error(name, "--" + key + " does not take a value") }
            if bool(o.get("repeatable", false)) {
               def current = _append_repeatable(values.get(key, nil), !negated)
               values[key] = current
            } else { values[key] = !negated }
         } else {
            if negated { return _error(name, "--no- is only valid for boolean options") }
            mut raw = ""
            if eq >= 0 { raw = str.str_slice(tok, eq + 1, tok.len) }
            elif bool(o.get("optional_value", false)) { raw = to_str(o.get("implicit_value", "")) }
            else {
               if i + 1 >= input.len { return _error(name, "--" + key + " requires a value") }
               def next = to_str(input.get(i + 1))
               if _looks_like_option(next) { return _error(name, "--" + key + " requires a value") }
               i += 1 raw = to_str(input.get(i))
            }
            def converted = _convert(raw, to_str(o.get("type", "str")), "--" + key)
            if !bool(converted.get("ok", false)) { return _error(name, to_str(converted.get("error", "invalid value"))) }
            if bool(o.get("repeatable", false)) {
               def current = _append_repeatable(values.get(key, nil), converted.get("value"))
               values[key] = current
            } else { values[key] = converted.get("value") }
         }
         seen[key] = true
         i += 1 continue
      }
      if i < boundary && tok.len > 1 && str.startswith(tok, "-") {
         def body = str.str_slice(tok, 1, tok.len)
         mut j = 0
         while j < body.len {
            def letter = chr(body.byte_at(j))
            def idx = _short_index(options, letter)
            if idx >= 0 {
               def o = options.get(idx)
               def long = to_str(o.get("long", ""))
               if to_str(o.get("kind", "")) == "flag" {
                  if bool(o.get("repeatable", false)) {
                     def current = _append_repeatable(values.get(long, nil), true)
                     values[long] = current
                  } else { values[long] = true }
                  seen[long] = true
                  j += 1
                  continue
               }
               mut raw = str.str_slice(body, j + 1, body.len)
               if raw.len == 0 {
                  if bool(o.get("optional_value", false)) { raw = to_str(o.get("implicit_value", "")) }
                  else {
                     if i + 1 >= input.len { return _error(name, "-" + letter + " requires a value") }
                     def next = to_str(input.get(i + 1))
                     if _looks_like_option(next) { return _error(name, "-" + letter + " requires a value") }
                     i += 1 raw = to_str(input.get(i))
                  }
               }
               def converted = _convert(raw, to_str(o.get("type", "str")), "--" + long)
               if !bool(converted.get("ok", false)) { return _error(name, to_str(converted.get("error", "invalid value"))) }
               if bool(o.get("repeatable", false)) {
                  def current = _append_repeatable(values.get(long, nil), converted.get("value"))
                  values[long] = current
               } else { values[long] = converted.get("value") }
               seen[long] = true
               j = body.len
               continue
            }
            def fam_idx = _family_option(families, letter)
            if fam_idx >= 0 {
               def fam = families.get(fam_idx)
               def items = fam.get("items", dict(0))
               mut selected = values.get(to_str(fam.get("long", "")), [])
               j += 1
               if j >= body.len { return _error(name, "family -" + letter + " requires one or more items") }
               while j < body.len {
                  def item_letter = chr(body.byte_at(j))
                  if !items.contains(item_letter) { return _error(name, "unknown -" + letter + " item '" + item_letter + "'") }
                  def item = to_str(items.get(item_letter))
                  if !selected.contains(item) { selected = selected.append(item) }
                  j += 1
               }
               values[to_str(fam.get("long", ""))] = selected
               break
            }
            mut candidates = []
            for o in options { def short = to_str(o.get("short", "")) if short.len > 0 { candidates = candidates.append("-" + short) } }
            return _error(name, "unknown option '-" + letter + "'" + (_nearest("-" + letter, candidates).len > 0 ? " (did you mean '" + _nearest("-" + letter, candidates) + "'?)" : ""))
         }
         i += 1 continue
      }
      def commands = spec.get("subcommands", dict(0))
      if is_dict(commands) && commands.contains(tok) {
         def command_spec = commands.get(tok, {}).get("spec", {})
         mut child_argv = [tok]
         mut rest_index = i + 1
         while rest_index < input.len { child_argv = child_argv.append(to_str(input.get(rest_index, ""))) rest_index += 1 }
         mut child_result = parse(command_spec, child_argv)
         child_result["command"] = tok
         return child_result
      }
      positional_values = positional_values.append(tok)
      i += 1
   }
   mut pi = 0
   for p in spec.get("positionals", []) {
      def pname = to_str(p.get("name", "arg"))
      if to_str(p.get("kind", "")) == "rest" {
         values[pname] = positional_values.slice(pi, positional_values.len)
         pi = positional_values.len
      } elif pi < positional_values.len {
         def converted = _convert(to_str(positional_values.get(pi)), to_str(p.get("type", "str")), pname)
         if !bool(converted.get("ok", false)) { return _error(name, to_str(converted.get("error", "invalid value"))) }
         values[pname] = converted.get("value")
         pi += 1
      } elif bool(p.get("required", false)) { return _error(name, "missing required argument '" + pname + "'") }
   }
   if pi < positional_values.len { return _error(name, "unexpected argument '" + to_str(positional_values.get(pi)) + "'") }
   for o in options {
      def long = to_str(o.get("long", ""))
      if bool(o.get("required", false)) &&
      !bool(seen.get(long, false)) &&
      !bool(env_satisfied.get(long, false)) {
         return _error(name, "missing required option '--" + long + "'")
      }
      if bool(values.get(long, false)) {
         def conflict = o.get("conflicts", [])
         for other in conflict { if bool(values.get(to_str(other), false)) { return _error(name, "--" + long + " conflicts with --" + to_str(other)) } }
      }
   }
   {"ok":true, "help":false, "values":values, "provided":seen, "positionals":positional_values, "command":""}
}

#main {
   mut spec = app("probe", "CLI parser self-test")
   spec = add(spec, flag("verbose", "v", "Enable verbose output"))
   spec = add(spec, option("cells", "c", "Tape cells", "int", 30000, false))
   spec = add(spec, flag("safe", "s", "Safe mode"))
   spec = add(spec, positional("file", "Input file", "path", false))
   spec = add(spec, rest("args"))
   spec = add(spec, family("dump", "D", {"n":"nyr", "v":"vm"}))
   def parsed = parse(spec, ["probe", "-vs", "--cells=12", "-Dnv", "input", "--", "-x"])
   assert(bool(parsed.get("ok", false)), "cli parser accepts flags, values, families, positionals and separator")
   def values = parsed.get("values", {})
   assert(bool(values.get("verbose", false)) && bool(values.get("safe", false)) && values.get("cells", 0) == 12, "cli parser values")
   assert(values.get("dump", []) == ["nyr", "vm"] && values.get("args", []) == ["-x"], "cli parser family and rest")
   def bad = parse(spec, ["probe", "--celss=abc"])
   assert(!bool(bad.get("ok", true)) && format_error(bad).contains("did you mean"), "cli parser unknown option suggestion")
   def typed = parse(spec, ["probe", "--cells=abc"])
   assert(!bool(typed.get("ok", true)) && format_error(typed).contains("expected int"), "cli parser typed error")
   assert(parse(spec, ["probe", "--no-safe"]).get("values", {}).get("safe", true) == false, "cli parser negation")
   print("✓ core.os.cli self-test passed")
}
