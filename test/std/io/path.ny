use std.io.path
use std.core

fn test_basename(){
	print("Testing basename...")
	assert(eq(basename("/foo/bar.txt"), "bar.txt"), "basename file")
	assert(eq(basename("/foo/bar/"), "bar"), "basename dir with slash")
	assert(eq(basename("bar"), "bar"), "basename plain")
	assert(eq(basename("/"), "/"), "basename root")
	assert(eq(basename(""), ""), "basename empty")
}

fn test_dirname(){
	print("Testing dirname...")
	assert(eq(dirname("/foo/bar.txt"), "/foo"), "dirname file")
	assert(eq(dirname("/foo/bar/"), "/foo"), "dirname dir with slash")
	assert(eq(dirname("bar"), "."), "dirname plain")
	assert(eq(dirname("/"), "/"), "dirname root")
	assert(eq(dirname("/foo"), "/"), "dirname root parent")
}

fn test_extname(){
	print("Testing extname...")
	assert(eq(extname("foo.txt"), ".txt"), "ext .txt")
	assert(eq(extname("/path/to/foo.tar.gz"), ".gz"), "ext .gz")
	assert(eq(extname("file"), ""), "ext none")
	assert(eq(extname(".hidden"), ".hidden"), "ext .hidden")
}

fn test_join(){
	print("Testing join...")
	assert(eq(path_join("foo", "bar"), "foo/bar"), "join simple")
	assert(eq(path_join("/foo", "bar"), "/foo/bar"), "join abs")
	assert(eq(path_join("foo/", "bar"), "foo/bar"), "join trailing")
	assert(eq(path_join("foo", "/bar"), "/bar"), "join absolute right")
}

fn test_main(){
	test_basename()
	test_dirname()
	test_extname()
	test_join()
	print("✓ std.io.path tests passed")
}

test_main()
