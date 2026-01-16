use std.io
use std.iter.itertools
use std.iter
use std.core
use std.core.test

print("Testing iter + itertools")

fn check_pair(p, i, a, b, msg){
	def pair = get(p, i)
	assert(get(pair, 0) == a, msg)
	assert(get(pair, 1) == b, msg)
}

fn test_itertools(){
	def a=[1,2] b=["a","b"]
	def p=product(a,b)
	assert(list_len(p)==4,"product len")
	check_pair(p, 0, 1, "a", "p0")
	check_pair(p, 1, 1, "b", "p1")
	check_pair(p, 2, 2, "a", "p2")
	check_pair(p, 3, 2, "b", "p3")
	assert(sum([1,2,3,4])==10,"sum")
	; Enumerate
	def e = enumerate(["x","y"])
	check_pair(e, 0, 0, "x", "enumerate0")
	check_pair(e, 1, 1, "y", "enumerate1")
	; Zip
	def z = zip([1,2,3],[4,5])
	check_pair(z, 0, 1, 4, "zip0")
	check_pair(z, 1, 2, 5, "zip1")
	fn inc(x){x+1}
	fn dbl(x){x*2}
	assert(compose(dbl,inc,3)==8,"compose")
	assert(iter_pipe(3,[inc,dbl,inc])==9,"iter_pipe")
}

fn test_iter(){
	fn even(x){x%2==0}
	fn add(acc,x){acc+x}
	def r=range(5)
	assert(list_len(r)==5, "range len")
	assert(get(r,0)==0, "r0")
	assert(get(r,4)==4, "r4")
	def r2 = range(2,5)
	assert(get(r2,0)==2, "range2_0")
	assert(get(r2,2)==4, "range2_2")
	; range(0,10,2) -> 0,2,4,6,8
	def r3=range(0,10,2)
	assert(get(r3,1)==2, "range3_1")
	; range(5,0,-1) -> 5,4,3,2,1
	def r4=range(5,0,-1)
	assert(get(r4,0)==5, "range4_0")
	assert(get(r4,4)==1, "range4_4")
	def m = map(r,fn(x){x*2})
	assert(get(m,0)==0, "map0")
	assert(get(m,1)==2, "map1")
	def f = filter(r,even)
	assert(get(f,0)==0, "filter0")
	assert(get(f,1)==2, "filter1")
	assert(reduce([1,2,3,4,5],fn(acc,x){acc+x},0)==15,"reduce")
}

test_itertools()
test_iter()

print("✓ all iter tests passed")
