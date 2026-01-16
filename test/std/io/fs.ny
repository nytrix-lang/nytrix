; Test std.io.fs - File system operations
use std.io.fs

; File read/write
fn test_main(){
	; File read/write
	def test_file = "/tmp/nytrix_io_test.txt"
	def test_data = "Test data for I/O"
	file_write(test_file, test_data)
	assert(file_exists(test_file), "file exists after write")
	def content = file_read(test_file)
	assert(eq(content, test_data), "read content matches written")
	file_remove(test_file)
	assert(!file_exists(test_file), "file removed")
	; File append
	test_file = "/tmp/nytrix_append_test.txt"
	file_write(test_file, "Line 1\n")
	file_append(test_file, "Line 2\n")
	file_append(test_file, "Line 3\n")
	content = file_read(test_file)
	assert(str_contains(content, "Line 1"), "contains line 1")
	assert(str_contains(content, "Line 2"), "contains line 2")
	assert(str_contains(content, "Line 3"), "contains line 3")
	file_remove(test_file)
	; File exists
	test_file = "/tmp/nytrix_exists_test.txt"
	assert(!file_exists(test_file), "file doesn't exist")
	file_write(test_file, "data")
	assert(file_exists(test_file), "file exists after creation")
	file_remove(test_file)
	assert(!file_exists(test_file), "file doesn't exist after removal")
}
test_main()
print("✓ std.io.fs tests passed")
