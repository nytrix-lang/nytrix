;;; mod.ny --- cli mod module

;; Keywords: cli mod

;;; Commentary:

;; Cli Mod module.

use std.strings.str
use std.core.reflect
use std.collections
module std.cli (
	argc, argv, args, has_flag, get_flag, parse_args, parse_argv
)

fn argc(){
	"Argument count."
	return rt_argc()  }

fn argv(i){
	"Argument pointer by index."
	return rt_argv(i)  }

fn args(){
	"All args as list of strings."
	def n = rt_argc()
	def xs = list(8)
	def i = 0
	while(i < n){
		xs = append(xs, rt_argv(i))
		i = i + 1
	}
	return xs
}

fn has_flag(flag){
	"Check if flag exists (exact ismatch)."
	def xs = args()
	def i = 0  n = list_len(xs)
	while(i < n){
		if(eq(get(xs, i), flag)==1){ return 1  }
		i = i + 1
	}
	return 0
}

fn get_flag(flag, default=0){
	"Get flag value: returns next arg or default."
	def xs = args()
	def i = 0  n = list_len(xs)
	while(i < n){
		if(eq(get(xs, i), flag)==1){
			if(i + 1 < n){ return get(xs, i+1)  }
			return default
		}
		i = i + 1
	}
	return default
}

fn parse_args(xs){
	"Parse args into {flags: dict, pos: list}."
	def flags = dict(16)
	def pos = list(8)
	def i =0  n=list_len(xs)
	while(i<n){
		def a = get(xs,i)
		if(startswith(a, "--")){
			if(len(a)==2){ pos = append(pos, a)  i=i+1  continue  }
			def eqi = find(a, "=")
			if(eqi >= 0){
				def k = strip(slice(a, 2, eqi))
				def v = strip(slice(a, eqi+1, len(a)))
				setitem(flags, k, v)
			} else {
				setitem(flags, slice(a, 2, len(a)), 1)
			}
		} else if(startswith(a, "-")){
			if(len(a)==2 && i+1<n && !startswith(get(xs,i+1), "-")){
				setitem(flags, slice(a,1,2), get(xs,i+1))
				i=i+1
			} else {
				def j =1
				while(j<len(a)){
					setitem(flags, slice(a,j,j+1), 1)
					j=j+1
				}
			}
		} else {
			pos = append(pos, a)
		}
		i=i+1
	}
	return {"flags": flags, "pos": pos}
}

fn parse_argv(){
	"Parse argv from runtime."
	return parse_args(args())
}
