;;; inspect.ny --- util inspect module

;; Keywords: util inspect

;;; Commentary:

;; Util Inspect module.

use std
module std.util.inspect (
	inspect
)

fn inspect(x){
	"Prints detailed information about value x."
	def t = type(x)
	print(f"Type:  {t}")
	print(f"Value: {repr(x)}")
	if(t == "list" || t == "dict" || t == "set"){
		print(f"Len:   {itoa(len(x))}")
	}
	if(is_ptr(x)){
		print(f"Addr:  0x{itoa(x)}") ; itoa doesn't do hex but we see large num
	}
	return 0
}
