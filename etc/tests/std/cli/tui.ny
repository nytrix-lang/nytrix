use std.io
use std.cli.tui
use std.core
use std.core.test
use std.strings.str

fn test_tui(){
	print("Testing TUI...")
	fn test_styling(){
		def s = "hello"
		assert(str_contains(bold(s), "\033[1m"), "bold start")
		assert(str_contains(bold(s), "\033[0m"), "bold end")
		assert(str_contains(bold(s), "hello"), "bold content")
		assert(str_contains(italic(s), "\033[3m"), "italic start")
		assert(str_contains(italic(s), "\033[0m"), "italic end")
		assert(str_contains(dim(s), "\033[2m"), "dim start")
		assert(str_contains(underline(s), "\033[4m"), "underline start")
		assert(str_contains(color(s, "red"), "\033[31m"), "color red")
		assert(str_contains(color(s, "green"), "\033[32m"), "color green")
		def styled = style(s, "blue", 1)
		assert(str_contains(styled, "\033[1m"), "style bold")
		assert(str_contains(styled, "\033[34m"), "style blue")
		assert(str_contains(styled, "\033[0m"), "style end")
	}
	test_styling()
	print("✓ std.cli.tui tests passed")
}

test_tui()
