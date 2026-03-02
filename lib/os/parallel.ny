;; Keywords: os parallel
;; Parallel CPU threading policy.

module std.os.parallel (
   parallel_mode, parallel_threads, parallel_min_work, parallel_should_threads, parallel_status,
   PARALLEL_MODE, PARALLEL_THREADS, PARALLEL_MIN_WORK
)

use std.core *
use std.text *
use std.os.prim *

fn _parse_threads(v){
   "Internal: parses non-negative values from strings."
   if(!is_str(v)){ return 0 }
   def s = strip(v)
   if(str_len(s) == 0){ return 0 }
   def n = atoi(s)
   if(n < 0){ return 0 }
   n
}

fn _normalize_parallel_mode(v){
   "Internal: normalizes parallel mode to `off|auto|threads`."
   if(!is_str(v)){ return "auto" }
   def s = lower(strip(v))
   if(eq(s, "off") || eq(s, "auto") || eq(s, "threads")){ return s }
   "auto"
}

fn _logical_cpu_guess(){
   "Internal: guesses host logical CPU count from environment hints."
   def n1 = _parse_threads(env("NYTRIX_LOGICAL_CPUS"))
   if(n1 > 0){ return n1 }
   def n2 = _parse_threads(env("NUMBER_OF_PROCESSORS"))
   if(n2 > 0){ return n2 }
   def n3 = _parse_threads(env("NPROC"))
   if(n3 > 0){ return n3 }
   2
}

mut _parallel_threads_eff_loaded = false
mut _parallel_threads_eff_cache = 0
mut _parallel_min_work_eff_loaded = false
mut _parallel_min_work_eff_cache = 0

fn _effective_parallel_threads(){
   "Internal: computes effective thread budget."
   if(_parallel_threads_eff_loaded){ return _parallel_threads_eff_cache }
   mut out = 2
   if(PARALLEL_THREADS > 0){ out = PARALLEL_THREADS }
   else {
      def n = _logical_cpu_guess()
      if(n > 1){ out = n }
   }
   _parallel_threads_eff_cache = out
   _parallel_threads_eff_loaded = true
   out
}

fn _effective_parallel_min_work(){
   "Internal: computes effective min workload for threaded parallel policy."
   if(_parallel_min_work_eff_loaded){ return _parallel_min_work_eff_cache }
   mut out = 65536
   if(PARALLEL_MIN_WORK > 0){ out = PARALLEL_MIN_WORK }
   else {
      def t = _effective_parallel_threads()
      if(t >= 8){ out = 262144 }
      elif(t >= 4){ out = 131072 }
   }
   _parallel_min_work_eff_cache = out
   _parallel_min_work_eff_loaded = true
   out
}

def PARALLEL_MODE = _normalize_parallel_mode(strip(to_str(env("NYTRIX_PARALLEL_MODE"))))
def PARALLEL_THREADS = _parse_threads(env("NYTRIX_PARALLEL_THREADS"))
def PARALLEL_MIN_WORK = _parse_threads(env("NYTRIX_PARALLEL_MIN_WORK"))

fn parallel_mode(){
   "Returns the configured parallel mode: `off`, `auto`, or `threads`.
   Configure via compiler CLI flag `--parallel`."
   PARALLEL_MODE
}

fn parallel_threads(){
   "Returns configured thread budget; `0` means runtime/default auto sizing.
   Configure via compiler CLI flag `--threads`."
   PARALLEL_THREADS
}

fn parallel_min_work(){
   "Returns minimum work threshold before selecting threaded parallel execution.
   Configure via compiler CLI flag `--parallel-min-work`."
   PARALLEL_MIN_WORK
}

fn parallel_status(work_items=0){
   "Returns a threading decision map for `work_items`."
   mut out = dict(16)
   def threads_eff = _effective_parallel_threads()
   def min_work_eff = _effective_parallel_min_work()
   mut selected = false
   mut reason = "cpu_default"
   if(PARALLEL_MODE == "off"){
      reason = "parallel_mode_off"
   } elif(threads_eff <= 1){
      reason = "single_thread_budget"
   } elif(work_items > 0 && work_items < min_work_eff){
      reason = "below_min_work"
   } else {
      selected = true
      reason = "eligible"
   }
   out = dict_set(out, "mode", PARALLEL_MODE)
   out = dict_set(out, "threads", PARALLEL_THREADS)
   out = dict_set(out, "effective_threads", threads_eff)
   out = dict_set(out, "min_work", PARALLEL_MIN_WORK)
   out = dict_set(out, "effective_min_work", min_work_eff)
   out = dict_set(out, "work_items", work_items)
   out = dict_set(out, "selected", selected)
   out = dict_set(out, "reason", reason)
   out
}

fn parallel_should_threads(work_items=0){
   "Returns true when thread-parallel policy selects threaded execution."
   dict_get(parallel_status(work_items), "selected", false)
}
