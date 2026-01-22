;; Keywords: cli mod
;; Cli Mod module.

module std.cli (
   argc, argv, args, contains_flag, get_flag, parse_args
)

fn argc(){
   "Returns the number of command-line arguments."
   __argc()
}

fn argv(i){
   "Returns the command-line argument string at index `i`."
   __argv(i)
}

fn args(){
   "Returns a [[std.core::list]] of all command-line arguments."
   def n = __argc()
   def xs = list(8)
   def i = 0
   while(i < n){
      xs = append(xs, __argv(i))
      i += 1
   }
   xs
}

fn contains_flag(flag){
   "Checks if the specific `flag` (e.g., '--verbose') is present in the command-line arguments."
   def xs = args()
   def i = 0  def n = list_len(xs)
   while(i < n){
      if(eq(get(xs, i), flag)){ return true }
      i += 1
   }
   false
}

fn get_flag(flag, default=0){
   "Retrieves the value associated with `flag`. Returns the next argument or `default` if the flag is missing or has no value."
   def xs = args()
   def i = 0  def n = list_len(xs)
   while(i < n){
      if(eq(get(xs, i), flag)){
         if(i + 1 < n){ return get(xs, i + 1) }
         break
      }
      i += 1
   }
   default
}

fn parse_args(xs){
   "Parses a list of arguments `xs` into a dictionary with 'flags' and 'pos' (positional arguments)."
   def flags = dict(16)
   def pos = list(8)
   def i = 0  def n = list_len(xs)
   while(i < n){
      def a = get(xs, i)
      if(startswith(a, "--")){
         if(len(a) == 2){
            pos = append(pos, a)
            i += 1
            continue
         }
         def eqi = find(a, "=")
         if(eqi >= 0){
            def k = strip(slice(a, 2, eqi))
            def v = strip(slice(a, eqi + 1, len(a)))
            dict_set(flags, k, v)
         } else {
            dict_set(flags, slice(a, 2, len(a)), 1)
         }
      } elif(startswith(a, "-")){
         if(len(a) == 2 && i + 1 < n && !startswith(get(xs, i+1), "-")){
            dict_set(flags, slice(a, 1, 2), get(xs, i+1))
            i += 1
         } else {
            def j = 1
            while(j < len(a)){
               dict_set(flags, slice(a, j, j + 1), 1)
               j += 1
            }
         }
      } else {
         pos = append(pos, a)
      }
      i += 1
   }
   return {"flags": flags, "pos": pos}
}