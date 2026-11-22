use core.math.parse.img.webp as webp

fn _webp_fixture(str hex) str {
   def bytes = hex.unhex
   def ptr = malloc(bytes.len + 32)
   mut i = 0
   while i < bytes.len {
      store8(ptr, bytes[i], i)
      i += 1
   }
   init_str(ptr, bytes.len)
}

fn test_webp_real_decode() {
   assert(webp.decode("not a WebP image") == 0)
   def lossless = _webp_fixture("524946463c000000574542505650384c300000002f024000102f201048da1f7a0d02491bd7fccd3fff049236361f4ffe80b913742628641b014eed1e8333bb88fec77a00")
   def img = webp.decode(lossless)
   assert(img, "VP8L decoder returned no pixels")
   if img {
      assert(img["width"] == 3 && img["height"] == 2)
      assert(img["channels"] == 4 && img["data"].len == 24)
      assert(load8(img["data"], 0) == 255 && load8(img["data"], 1) == 0)
      assert(load8(img["data"], 6) == 0 && load8(img["data"], 10) == 255)
      assert(load8(img["data"], 11) == 128 && load8(img["data"], 23) == 0)
   }
   def lossy = _webp_fixture("52494646700000005745425056503820640000009002009d012a0300020000c01225a80274470064807e8002dd9f393a8000fefe2e7ff886fffed7ff8eb1ffbfce9f7fddfa2d32ffd6121fbc4a8898225deffeaf4ffc53cae91d3fffc80ffffabd3ff14f2ba474ffc5d1fe78e213bb507e78e21388c00000")
   def rgb = webp.decode(lossy)
   if rgb {
      assert(rgb["width"] == 3 && rgb["height"] == 2)
      assert(rgb["channels"] == 4 && rgb["data"].len == 24)
   }
}

#main {
   test_webp_real_decode()
   print("webp vectors ok")
}
