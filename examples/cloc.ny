;; Nytrix Line of Code Counter
use std.io
use std.io.fs
use std.strings.str
use std.core

fn count_lines(path){
	def src = file_read(path)
	if(str_len(src) == 0){ return [0, 0] }
	def code = 0
	def cmts = 0
	def lines = split(src, "\n")
	def i = 0
	while(i < len(lines)){
		def l = strip(get(lines, i))
		if(len(l) > 0){
			if(load8(l) == 34 || load8(l) == 59){
				cmts = cmts + 1
			} else {
				code = code + 1
			}
		}
		i = i + 1
	}
	return [code, cmts]
}

fn collect_files(dir, out_list){
	def items = listdir(dir)
	if(!items){ return out_list }
	def i = 0
	while(i < len(items)){
		def name = get(items, i)
		if(eq(name, ".") || eq(name, "..") || eq(name, ".git") || eq(name, "build")){
			i = i + 1
			continue
		}
		def full = concat(concat(dir, "/"), name)
		if(is_dir(full)){
			out_list = collect_files(full, out_list)
		} else {
			if(endswith(full, ".ny") || endswith(full, ".c") || endswith(full, ".h")){
				out_list = append(out_list, full)
			}
		}
		i = i + 1
	}
	return out_list
}

print(concat(pad_end("File", 40), " Code  Comments"))

def all_files = list(8)
all_files = collect_files(".", all_files)

def total_code = 0
def total_cmts = 0

def i = 0
while(i < len(all_files)){
	def p = get(all_files, i)
	def res = count_lines(p)
	def c = get(res, 0)
	def m = get(res, 1)
	total_code = total_code + c
	total_cmts = total_cmts + m
	print(concat(concat(concat(pad_end(p, 40), " "), itoa(c)), concat("    ", itoa(m))))
	i = i + 1
}

print(concat(concat(concat(pad_end("TOTAL", 40), " "), itoa(total_code)), concat("    ", itoa(total_cmts))))
