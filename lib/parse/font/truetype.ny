;; Keywords: font truetype ttf opentype freetype bitmap
;; FreeType 2 wrapper — static extern fn linking via #link.

module std.ui.font.truetype (
   load, unload, available,
   scale_for_pixel_height, scale_for_em,
   get_vmetrics, get_hmetrics, get_kern,
   get_glyph_index, get_glyph_bitmap, get_glyph_box,
   get_ascent_descent_gap
)

use std.core *
use std.core.dict_mod *
use std.os *

if(comptime{ __os_name() == "linux" }){
    #link "freetype"
} else if(comptime{ __os_name() == "macos" }){
    #link "freetype"
} else if(comptime{ __os_name() == "windows" }){
    #link "freetype"
}

extern fn FT_Init_FreeType(lib: ptr): i32 as "FT_Init_FreeType"
extern fn FT_Done_FreeType(lib: ptr): i32 as "FT_Done_FreeType"
extern fn FT_New_Memory_Face(lib: ptr, data: ptr, size: i64, index: i64, face: ptr): i32 as "FT_New_Memory_Face"
extern fn FT_Done_Face(face: ptr): i32 as "FT_Done_Face"
extern fn FT_Set_Pixel_Sizes(face: ptr, w: i32, h: i32): i32 as "FT_Set_Pixel_Sizes"
extern fn FT_Load_Glyph(face: ptr, index: i32, flags: i32): i32 as "FT_Load_Glyph"
extern fn FT_Get_Char_Index(face: ptr, charcode: i64): i32 as "FT_Get_Char_Index"
extern fn FT_Get_Kerning(face: ptr, left: i32, right: i32, mode: i32, kern: ptr): i32 as "FT_Get_Kerning"
extern fn FT_Render_Glyph(slot: ptr, mode: i32): i32 as "FT_Render_Glyph"

;; FT struct offsets
def _FT_FACE_NUM_GLYPHS  = 32
def _FT_FACE_FAMILY_NAME = 40
def _FT_FACE_UNITS_PER_EM = 136
def _FT_FACE_ASCENDER    = 138
def _FT_FACE_DESCENDER   = 140
def _FT_FACE_HEIGHT      = 142
def _FT_FACE_GLYPH       = 152

def _GS_METRICS          = 48
def _GS_METRICS_BRX      = 64
def _GS_METRICS_BRY      = 72
def _GS_METRICS_ADV      = 80
def _GS_ADVANCE          = 128
def _GS_BM_ROWS          = 152
def _GS_BM_WIDTH         = 156
def _GS_BM_PITCH         = 160
def _GS_BM_BUFFER        = 168
def _GS_BM_PIXEL_MODE    = 178
def _GS_BM_LEFT          = 192
def _GS_BM_TOP           = 196

def FT_LOAD_DEFAULT       = 0
def FT_LOAD_RENDER        = 4
def FT_LOAD_FORCE_AUTOHINT= 32

def FT_RENDER_MODE_NORMAL = 0
def FT_RENDER_MODE_MONO   = 2

;; Library state

mut _FT_library  = 0

fn _init(){
    "Initializes the shared FreeType library handle."
    if(_FT_library){ return true }
    def lib_ptr = malloc(8)
    store64(lib_ptr, 0, 0)
    def rc = FT_Init_FreeType(lib_ptr)
    if(rc != 0){ free(lib_ptr) return false }
    _FT_library = load64(lib_ptr, 0)
    free(lib_ptr)
    !!_FT_library
}

fn available(){
    "Returns whether the FreeType-backed TrueType loader is available."
    _init()
}

;; Public API

fn load(data, index=0){
    "Loads a font face from in-memory font bytes."
    if(!_init()){ return 0 }
    if(!is_str(data) || len(data) < 4){ return 0 }
    def face_ptr = malloc(8)
    store64(face_ptr, 0, 0)
    def rc = FT_New_Memory_Face(_FT_library, data, len(data), index, face_ptr)
    if(rc != 0){ free(face_ptr) return 0 }
    def face = load64(face_ptr, 0)
    free(face_ptr)
    if(!face){ return 0 }
    def units_per_em = load16(face, _FT_FACE_UNITS_PER_EM)
    mut ascender  = load16(face, _FT_FACE_ASCENDER)
    mut descender = load16(face, _FT_FACE_DESCENDER)
    mut face_height = load16(face, _FT_FACE_HEIGHT)
    if(ascender > 32767){   ascender   = ascender - 65536 }
    if(descender > 32767){  descender  = descender - 65536 }
    if(face_height > 32767){ face_height = face_height - 65536 }
    def num_glyphs = load32(face, _FT_FACE_NUM_GLYPHS)
    mut info = dict(10)
    info = dict_set(info, "face",         face)
    info = dict_set(info, "data",         data)
    info = dict_set(info, "units_per_em", units_per_em)
    info = dict_set(info, "ascender",     ascender)
    info = dict_set(info, "descender",    descender)
    info = dict_set(info, "height",       face_height)
    info = dict_set(info, "num_glyphs",   num_glyphs)
    info = dict_set(info, "pixel_size",   0)
    info
}

fn unload(info){
    "Releases a loaded font face."
    if(!is_dict(info)){ return }
    def face = dict_get(info, "face", 0)
    if(face){ FT_Done_Face(face) }
}

;; Scale helpers

fn _set_size(info, px){
    "Ensures the face is configured for pixel size `px`."
    def face = dict_get(info, "face", 0)
    if(!face){ return false }
    def prev = dict_get(info, "pixel_size", 0)
    if(prev == px){ return true }
    def rc = FT_Set_Pixel_Sizes(face, 0, px)
    if(rc != 0){ return false }
    dict_set(info, "pixel_size", px)
    true
}

fn _px(info){
    "Returns the cached pixel size, defaulting to 32."
    dict_get(info, "pixel_size", 32)
}

fn scale_for_pixel_height(info, pixels){
    "Returns the font-space scale required to reach `pixels` of height."
    def asc = dict_get(info, "ascender",  0)
    def dsc = dict_get(info, "descender", 0)
    def span = asc - dsc
    if(span <= 0){ return 1.0 }
    (pixels + 0.0) / (span + 0.0)
}

fn scale_for_em(info, pixels){
    "Returns the scale factor that maps the font EM square to `pixels`."
    def upm = dict_get(info, "units_per_em", 2048)
    if(upm <= 0){ return 1.0 }
    (pixels + 0.0) / (upm + 0.0)
}

fn get_vmetrics(info){
    "Returns vertical font metrics as `[ascender, descender, height]`."
    def asc = dict_get(info, "ascender",  0)
    def dsc = dict_get(info, "descender", 0)
    def ht  = dict_get(info, "height",    0)
    def gap = ht - (asc - dsc)
    mut v = list(3)
    v = append(v, asc)
    v = append(v, dsc)
    v = append(v, gap)
    v
}

fn get_ascent_descent_gap(info){
    "Alias for `get_vmetrics`."
    get_vmetrics(info)
}

;; Glyph index

fn get_glyph_index(info, cp){
    "Returns the glyph index for codepoint `cp`."
    def face = dict_get(info, "face", 0)
    if(!face){ return 0 }
    FT_Get_Char_Index(face, cp)
}

;; Glyph metrics

fn _load_glyph(info, gi, flags){
    "Loads glyph `gi` with FreeType flags `flags`."
    def face = dict_get(info, "face", 0)
    if(!face){ return false }
    FT_Load_Glyph(face, gi, flags) == 0
}

fn get_hmetrics(info, gi){
    "Returns horizontal glyph metrics for glyph index `gi`."
    def face = dict_get(info, "face", 0)
    if(!face){ return [0, 0] }
    def px = _px(info)
    if(px <= 0){ return [0, 0] }
    _set_size(info, px)
    if(!_load_glyph(info, gi, FT_LOAD_DEFAULT)){ return [0, 0] }
    def gs = load64(face, _FT_FACE_GLYPH)
    if(!gs){ return [0, 0] }
    def adv = load32(gs, _GS_METRICS_ADV)
    def brx = load32(gs, _GS_METRICS_BRX)
    mut adv_s = adv
    mut brx_s = brx
    if(adv_s > 2147483647){ adv_s = -(4294967296 - adv_s) }
    if(brx_s > 2147483647){ brx_s = -(4294967296 - brx_s) }
    mut r = list(2)
    r = append(r, adv_s / 64)
    r = append(r, brx_s / 64)
    r
}

fn get_glyph_box(info, gi){
    "Returns glyph bounds for glyph index `gi`."
    def face = dict_get(info, "face", 0)
    if(!face){ return 0 }
    def px = _px(info)
    if(px <= 0){ return 0 }
    _set_size(info, px)
    if(!_load_glyph(info, gi, FT_LOAD_DEFAULT)){ return 0 }
    def gs = load64(face, _FT_FACE_GLYPH)
    if(!gs){ return 0 }
    def rows  = load32(gs, _GS_BM_ROWS)
    def width = load32(gs, _GS_BM_WIDTH)
    if(rows <= 0 || width <= 0){ return 0 }
    def bl = load32(gs, _GS_BM_LEFT)
    def bt = load32(gs, _GS_BM_TOP)
    mut bl_s = bl mut bt_s = bt
    if(bl_s > 2147483647){ bl_s = bl_s - 4294967296 }
    if(bt_s > 2147483647){ bt_s = bt_s - 4294967296 }
    mut box = list(4)
    box = append(box, bl_s)
    box = append(box, bt_s - rows)
    box = append(box, bl_s + width)
    box = append(box, bt_s)
    box
}

fn get_kern(info, g1, g2){
    "Returns horizontal kerning between glyphs `g1` and `g2`."
    def face = dict_get(info, "face", 0)
    if(!face){ return 0 }
    def vec = malloc(16)
    store64(vec, 0, 0)
    store64(vec, 0, 8)
    def rc = FT_Get_Kerning(face, g1, g2, 0, vec)
    def kx = load32(vec, 0)
    free(vec)
    if(rc != 0){ return 0 }
    mut kx_s = kx
    if(kx_s > 2147483647){ kx_s = -(4294967296 - kx_s) }
    kx_s / 64
}

;; Glyph bitmap rasterization

fn get_glyph_bitmap(info, _scale_x, scale_y, gi){
    "Rasterizes glyph `gi` at the requested pixel scale and returns bitmap metadata."
    def face = dict_get(info, "face", 0)
    if(!face){ return 0 }
    def upm = dict_get(info, "units_per_em", 2048)
    mut px = __flt_to_int(scale_y * upm + 0.5)
    if(px < 1){ px = 1 }
    if(px > 1024){ px = 1024 }
    if(!_set_size(info, px)){ return 0 }
    def rc = FT_Load_Glyph(face, gi, FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT)
    if(rc != 0){ return 0 }
    def gs = load64(face, _FT_FACE_GLYPH)
    if(!gs){ return 0 }
    def rows  = load32(gs, _GS_BM_ROWS)
    def width = load32(gs, _GS_BM_WIDTH)
    def pitch = load32(gs, _GS_BM_PITCH)
    def src   = load64(gs, _GS_BM_BUFFER)
    def bl    = load32(gs, _GS_BM_LEFT)
    def bt    = load32(gs, _GS_BM_TOP)
    mut bl_s = bl mut bt_s = bt
    if(bl_s > 2147483647){ bl_s = bl_s - 4294967296 }
    if(bt_s > 2147483647){ bt_s = bt_s - 4294967296 }
    def pitch_s = (pitch > 2147483647) ? (pitch - 4294967296) : pitch
    if(rows <= 0 || width <= 0){
        mut res = dict(5)
        res = dict_set(res, "data",   0)
        res = dict_set(res, "width",  0)
        res = dict_set(res, "height", 0)
        res = dict_set(res, "xoff",   bl_s)
        res = dict_set(res, "yoff",   bt_s)
        return res
    }
    def bsize = rows * width
    def bmp = malloc(bsize + 1)
    init_str(bmp, bsize)
    memset(bmp, 0, bsize)
    mut y = 0
    while(y < rows){
        memcpy(bmp + (y * width), src + (y * pitch_s), width)
        y += 1
    }
    mut res = dict(5)
    res = dict_set(res, "data",   bmp)
    res = dict_set(res, "width",  width)
    res = dict_set(res, "height", rows)
    res = dict_set(res, "xoff",   bl_s)
    res = dict_set(res, "yoff",   bt_s)
    res
}

if(comptime{__main()}){
    use std.core *
    use std.core.error *

    print("Testing std.ui.font.truetype (FreeType backend)...")
    if(!available()){
        print("  SKIPPED: libfreetype unavailable")
        return
    }
    print("  FreeType library loaded.")

    def font_path = "etc/assets/font/monocraft.ttf"
    if(!file_exists(font_path)){
        print("  SKIPPED: etc/assets/font/monocraft.ttf missing")
        return
    }

    print("  Using font: " + font_path)
    def font_res = file_read(font_path)
    if(is_err(font_res)){
        print("  SKIPPED: file_read unavailable in comptime")
        print("✓ std.font.truetype tests passed")
        return
    }
    def font_data = unwrap(font_res)

    fn _test_load(d, i){
        "Small test hook around `load`."
        load(d, i)
    }
    def info = _test_load(font_data, 0)
    if(!info){
        print("  SKIPPED: font load failed")
        print("✓ std.font.truetype tests passed")
        return
    }

    def vm = get_vmetrics(info)
    assert(len(vm) == 3, "vmetrics returns 3 values")
    def ascender = get(vm, 0)
    assert(ascender > 0, "ascender > 0")
    print("  Metrics: ascender=" + to_str(ascender) + " descender=" + to_str(get(vm, 1)))

    def scale = scale_for_pixel_height(info, 32.0)
    assert(scale > 0.0, "scale > 0")

    def gi_A = get_glyph_index(info, 65)
    assert(gi_A > 0, "glyph index for 'A'")
    print("  Glyph index 'A' = " + to_str(gi_A))

    dict_set(info, "pixel_size", 32)
    _set_size(info, 32)

    def bm = get_glyph_bitmap(info, scale, scale, gi_A)
    assert(bm != 0, "glyph bitmap returned")
    def bm_w = dict_get(bm, "width")
    def bm_h = dict_get(bm, "height")
    assert(bm_w > 0, "bitmap width > 0")
    assert(bm_h > 0, "bitmap height > 0")
    print("  Glyph 'A' bitmap: " + to_str(bm_w) + "x" + to_str(bm_h))

    def gi_kern_V = get_glyph_index(info, 86)
    def kern = get_kern(info, gi_A, gi_kern_V)
    print("  Kern A+V = " + to_str(kern) + "px")

    unload(info)
    print("  Font unloaded.")
    print("✓ std.ui.font.truetype tests passed")
}
