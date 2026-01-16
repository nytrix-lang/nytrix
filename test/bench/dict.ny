use std.io
use std.collections.dict
use std.core
use std.strings.str

print("Debug Dict")
def d = dict()
print("Created dict")
use std.os.time

def t_start = ticks()
def i = 0
while(i < 1000){
	d = setitem(d, itoa(i), i)
	i = i + 1
}
def t_mid = ticks()

print("Insert (100k items) took (ns): ", t_mid - t_start)

def idx = 0
while(idx < 1000){
	getitem(d, itoa(idx), -1)
	idx = idx + 1
}
def t_end = ticks()
print("Lookup (100k items) took (ns): ", t_end - t_mid)
print("Time (ns): ", t_end - t_start)
