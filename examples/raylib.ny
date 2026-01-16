;; Testing Raylib FFI Using Low Level ffi functions ...
use std.os.ffi

def h = ffi.dlopen("/usr/lib/libraylib.so", 2)
if(h != 0){
	def InitWindow        = ffi.dlsym(h, "InitWindow")
	def WindowShouldClose = ffi.dlsym(h, "WindowShouldClose")
	def BeginDrawing      = ffi.dlsym(h, "BeginDrawing")
	def EndDrawing        = ffi.dlsym(h, "EndDrawing")
	def ClearBackground   = ffi.dlsym(h, "ClearBackground")
	def CloseWindow       = ffi.dlsym(h, "CloseWindow")
	assert(InitWindow != 0)
	assert(WindowShouldClose != 0)
	ffi.call3_void(InitWindow, 800, 450, "Nytrix")
	while(ffi.call0(WindowShouldClose) == 0){
		ffi.call0_void(BeginDrawing)
		ffi.call1_void(ClearBackground, 0xFF181818)
		ffi.call0_void(EndDrawing)
	}
	ffi.call0_void(CloseWindow)
	ffi.dlclose(h)
	print("✓ Raylib window closed")
} else {
	print("Raylib not found")
}
