;; Keywords: render image env
;; Environment-map and image preparation routines for rendering workflows.
module std.os.ui.render.img.env(srgb_to_linear_chan, linear_to_srgb_chan, linear_to_srgb_u8, image_sample_linear_rgb_uv, env_dir_to_uv, generate_spec_env_slab, generate_env_image, generate_neutral_env_image, generate_compare_visible_env_image, generate_compare_reflect_env_image, generate_studio_env_image, scene_prefers_studio_env, scene_prefers_neutral_env, scene_prefers_compare_reflect_env, scene_prefers_compare_visible_env, scene_prefers_optical_spec_env, scene_prefers_black_visible_env, scene_prefers_gray_proof_bg)
use std.core
use std.core.str as str
use std.math
use std.math.float (is_nan, is_inf)

@jit
fn _v3_norm(any: v): list {
   def x = float(v.get(0, 0.0))
   def y = float(v.get(1, 0.0))
   def z = float(v.get(2, 0.0))
   def l = sqrt(x * x + y * y + z * z)
   if(l <= 0.000000001){ return [0.0, 0.0, 0.0] }
   [x / l, y / l, z / l]
}

@jit
fn _v3_dot(any: a, any: b): f64 {
   float(a.get(0, 0.0)) * float(b.get(0, 0.0)) +
   float(a.get(1, 0.0)) * float(b.get(1, 0.0)) +
   float(a.get(2, 0.0)) * float(b.get(2, 0.0))
}

@jit
fn _v3_cross(any: a, any: b): list {
   def ax, ay = float(a.get(0, 0.0)), float(a.get(1, 0.0))
   def az = float(a.get(2, 0.0))
   def bx, by = float(b.get(0, 0.0)), float(b.get(1, 0.0))
   def bz = float(b.get(2, 0.0))
   [ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx]
}

@jit
fn srgb_to_linear_chan(any: x): f64 {
   def c = clamp(float(x), 0.0, 1.0)
   if(c <= 0.04045){ return c / 12.92 }
   pow((c + 0.055) / 1.055, 2.4)
}

@jit
fn linear_to_srgb_chan(any: x): f64 {
   mut c = float(x)
   if(is_nan(c)){ c = 0.0 }
   if(c < 0.0){ c = 0.0 } elif(c > 1.0){ c = 1.0 }
   if(c <= 0.0031308){ return c * 12.92 }
   1.055 * pow(c, 1.0 / 2.4) - 0.055
}

@jit
fn linear_to_srgb_u8(any: x): int {
   def y = linear_to_srgb_chan(x)
   if(is_nan(y)){ return 0 }
   clamp(int(y * 255.0 + 0.5), 0, 255)
}

@jit
fn image_sample_linear_rgb_uv(any: im, any: u, any: v): list {
   if(!is_dict(im)){ return [0.0, 0.0, 0.0] }
   def data = im.get("data", 0)
   def w = int(im.get("width", 0))
   def h = int(im.get("height", 0))
   if(!data || !is_str(data) || w <= 0 || h <= 0){ return [0.0, 0.0, 0.0] }
   mut uu = float(u) - floor(float(u))
   mut vv = clamp(float(v), 0.0, 1.0)
   def fx, fy = uu * float(w) - 0.5, vv * float(h) - 0.5
   mut x0, y0 = int(floor(fx)), int(floor(fy))
   mut x1, y1 = x0 + 1, y0 + 1
   def tx, ty = fx - float(x0), fy - float(y0)
   while(x0 < 0){ x0 += w }
   while(x1 < 0){ x1 += w }
   x0, x1 = x0 % w, x1 % w
   if(y0 < 0){ y0 = 0 }
   if(y1 < 0){ y1 = 0 }
   if(y0 >= h){ y0 = h - 1 }
   if(y1 >= h){ y1 = h - 1 }
   def i00, i10 = ((y0 * w) + x0) * 4, ((y0 * w) + x1) * 4
   def i01, i11 = ((y1 * w) + x0) * 4, ((y1 * w) + x1) * 4
   def c00 = [
      srgb_to_linear_chan(float(load8(data, i00 + 0) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i00 + 1) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i00 + 2) & 255) / 255.0)
   ]
   def c10 = [
      srgb_to_linear_chan(float(load8(data, i10 + 0) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i10 + 1) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i10 + 2) & 255) / 255.0)
   ]
   def c01 = [
      srgb_to_linear_chan(float(load8(data, i01 + 0) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i01 + 1) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i01 + 2) & 255) / 255.0)
   ]
   def c11 = [
      srgb_to_linear_chan(float(load8(data, i11 + 0) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i11 + 1) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i11 + 2) & 255) / 255.0)
   ]
   def a0 = [
      c00.get(0, 0.0) + (c10.get(0, 0.0) - c00.get(0, 0.0)) * tx,
      c00.get(1, 0.0) + (c10.get(1, 0.0) - c00.get(1, 0.0)) * tx,
      c00.get(2, 0.0) + (c10.get(2, 0.0) - c00.get(2, 0.0)) * tx
   ]
   def a1 = [
      c01.get(0, 0.0) + (c11.get(0, 0.0) - c01.get(0, 0.0)) * tx,
      c01.get(1, 0.0) + (c11.get(1, 0.0) - c01.get(1, 0.0)) * tx,
      c01.get(2, 0.0) + (c11.get(2, 0.0) - c01.get(2, 0.0)) * tx
   ]
   [
      a0.get(0, 0.0) + (a1.get(0, 0.0) - a0.get(0, 0.0)) * ty,
      a0.get(1, 0.0) + (a1.get(1, 0.0) - a0.get(1, 0.0)) * ty,
      a0.get(2, 0.0) + (a1.get(2, 0.0) - a0.get(2, 0.0)) * ty
   ]
}

@jit
fn env_dir_to_uv(any: d): list {
   def n = _v3_norm(d)
   def x = float(n.get(0, 0.0))
   def y, z = float(n.get(1, 0.0)), float(n.get(2, 0.0))
   def u_raw = 0.5 + atan2(x, 0.0 - z) / 6.283185307179586
   def v = clamp(0.5 - asin(clamp(y, -1.0, 1.0)) / 3.141592653589793, 0.00001, 0.99999)
   [u_raw - floor(u_raw), v]
}

@jit
fn _radical_inverse_vdc32(any: bits0): f64 {
   mut bits = int(bits0)
   bits = ((bits << 16) | ((bits >> 16) & 0xffff)) & 0xffffffff
   bits = (((bits & 0x55555555) << 1) | ((bits & 0xaaaaaaaa) >> 1)) & 0xffffffff
   bits = (((bits & 0x33333333) << 2) | ((bits & 0xcccccccc) >> 2)) & 0xffffffff
   bits = (((bits & 0x0f0f0f) << 4) | ((bits & 0xf0f0f0) >> 4)) & 0xffffffff
   bits = (((bits & 0x00ff00ff) << 8) | ((bits & 0xff00ff00) >> 8)) & 0xffffffff
   float(bits) * 2.3283064365386963e-10
}

@jit
fn _importance_sample_ggx(any: xi_x, any: xi_y, any: roughness, any: N): list {
   def a = roughness * roughness
   def a2 = a * a
   def phi = 6.283185307179586 * xi_x
   def cos_theta = sqrt((1.0 - xi_y) / max(1.0 + (a2 - 1.0) * xi_y, 1e-6))
   def sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0))
   def H = [cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta]
   def Nz = float(N.get(2, 0.0))
   def up = (abs(Nz) < 0.999) ? [0.0, 0.0, 1.0] : [1.0, 0.0, 0.0]
   def tangent = _v3_norm(_v3_cross(up, N))
   def bitangent = _v3_cross(N, tangent)
   _v3_norm([
         tangent.get(0, 0.0) * H.get(0, 0.0) + bitangent.get(0, 0.0) * H.get(1, 0.0) + float(N.get(0, 0.0)) * H.get(2, 0.0),
         tangent.get(1, 0.0) * H.get(0, 0.0) + bitangent.get(1, 0.0) * H.get(1, 0.0) + float(N.get(1, 0.0)) * H.get(2, 0.0),
         tangent.get(2, 0.0) * H.get(0, 0.0) + bitangent.get(2, 0.0) * H.get(1, 0.0) + float(N.get(2, 0.0)) * H.get(2, 0.0)
   ])
}

fn generate_spec_env_slab(any: im, int: base_w=256): any {
   if(!is_dict(im)){ return 0 }
   def src_w, src_h = int(im.get("width", 0)), int(im.get("height", 0))
   if(src_w <= 0 || src_h <= 0){ return 0 }
   mut w0 = clamp(int(base_w), 64, 512)
   if(w0 > src_w){ w0 = src_w }
   def h0 = max(1, w0 / 2)
   mut levels = 1
   mut tw = w0
   mut th = h0
   mut total = 0
   while(true){
      total += tw * th * 4
      if(tw <= 1 && th <= 1){ break }
      tw, th = max(1, tw >> 1), max(1, th >> 1)
      levels += 1
   }
   def slab = malloc(total)
   if(!slab){ return 0 }
   mut off = 0
   mut level = 0
   while(level < levels){
      def w, h = max(1, w0 >> level), max(1, h0 >> level)
      def roughness = (levels > 1) ? float(level) / float(levels - 1) : 0.0
      mut sample_count = 1
      if(level > 0){
         if(roughness < 0.15){ sample_count = 64 }
         elif(roughness < 0.5){ sample_count = 32 }
         else { sample_count = 16 }
      }
      mut y = 0
      while(y < h){
         def vv = (float(y) + 0.5) / float(h)
         def elev = (0.5 - vv) * 3.141592653589793
         def sin_e = sin(elev)
         def cos_e = cos(elev)
         mut x = 0
         while(x < w){
            def uu = (float(x) + 0.5) / float(w)
            def phi = (uu - 0.5) * 6.283185307179586
            def N = _v3_norm([cos_e * cos(phi), sin_e, cos_e * sin(phi)])
            mut c0, c1 = 0.0, 0.0
            mut c2 = 0.0
            mut weight = 0.0
            if(roughness <= 0.0 || sample_count <= 1){
               def uv = env_dir_to_uv(N)
               def s = image_sample_linear_rgb_uv(im, uv.get(0, 0.0), uv.get(1, 0.0))
               c0, c1 = s.get(0, 0.0), s.get(1, 0.0)
               c2 = s.get(2, 0.0)
               weight = 1.0
            } else {
               mut i = 0
               while(i < sample_count){
                  def xi_x, xi_y = float(i) / float(sample_count), _radical_inverse_vdc32(i)
                  def H = _importance_sample_ggx(xi_x, xi_y, roughness, N)
                  def VoH = max(_v3_dot(N, H), 0.0)
                  def L = _v3_norm([
                        2.0 * VoH * H.get(0, 0.0) - N.get(0, 0.0),
                        2.0 * VoH * H.get(1, 0.0) - N.get(1, 0.0),
                        2.0 * VoH * H.get(2, 0.0) - N.get(2, 0.0)
                  ])
                  def NoL = max(_v3_dot(N, L), 0.0)
                  if(NoL > 0.0){
                     def uv = env_dir_to_uv(L)
                     def s = image_sample_linear_rgb_uv(im, uv.get(0, 0.0), uv.get(1, 0.0))
                     c0 += s.get(0, 0.0) * NoL
                     c1 += s.get(1, 0.0) * NoL
                     c2 += s.get(2, 0.0) * NoL
                     weight += NoL
                  }
                  i += 1
               }
            }
            if(weight > 0.0){
               c0, c1 = c0 / weight, c1 / weight
               c2 = c2 / weight
            }
            def dp = off + ((y * w) + x) * 4
            store8(slab, linear_to_srgb_u8(c0), dp + 0)
            store8(slab, linear_to_srgb_u8(c1), dp + 1)
            store8(slab, linear_to_srgb_u8(c2), dp + 2)
            store8(slab, 255, dp + 3)
            x += 1
         }
         y += 1
      }
      off += w * h * 4
      level += 1
   }
   mut out = dict(8)
   out["pixels"] = slab
   out["width"] = w0
   out["height"] = h0
   out["levels"] = levels
   out["bytes"] = total
   out
}

fn _rgba_image_result(any: pixels, int: w, int: h): dict {
   mut out = dict(8)
   out["data"] = init_str(pixels, w * h * 4)
   out["width"] = w
   out["height"] = h
   out["channels"] = 4
   out
}

fn generate_env_image(int: kind=0, int: w=1024, int: h=512): any {
   def iw, ih = max(1, int(w)), max(1, int(h))
   def pixels = malloc(iw * ih * 4)
   if(!pixels){ return 0 }
   def fw, fh = float(iw), float(ih)
   mut y = 0
   while(y < ih){
      def v = (float(y) + 0.5) / fh
      def elev = (0.5 - v) * 3.141592653589793
      def dy = sin(elev)
      def sky_t = clamp(dy * 0.5 + 0.5, 0.0, 1.0)
      def top_t = clamp(dy, 0.0, 1.0)
      def floor_t = clamp(-dy, 0.0, 1.0)
      def row = y * iw * 4
      mut x = 0
      while(x < iw){
         def u = (float(x) + 0.5) / fw
         mut c0, c1 = 0.0, 0.0
         mut c2 = 0.0
         if(kind == 0){
            c0, c1 = (0.58 * (1.0 - sky_t)) + ((0.78 + 0.035 * top_t) * sky_t), (0.60 * (1.0 - sky_t)) + ((0.80 + 0.036 * top_t) * sky_t)
            c2 = (0.66 * (1.0 - sky_t)) + ((0.86 + 0.040 * top_t) * sky_t)
            def key1_dx, key1_dy = (u - 0.74) / 0.055, (v - 0.27) / 0.085
            def key1 = exp(-(key1_dx * key1_dx + key1_dy * key1_dy))
            def key2_dx = (u - 0.28) / 0.070
            def key2_dy = (v - 0.30) / 0.095
            def key2 = exp(-(key2_dx * key2_dx + key2_dy * key2_dy))
            def top_strip = exp(-pow((v - 0.17) / 0.040, 2.0)) * exp(-pow((u - 0.50) / 0.32, 6.0))
            def horizon = exp(-pow((v - 0.50) / 0.11, 2.0)) * 0.012
            c0 += key1 * 0.10 + key2 * 0.08 + top_strip * 0.034 + horizon
            c1 += key1 * 0.10 + key2 * 0.08 + top_strip * 0.034 + horizon
            c2 += key1 * 0.11 + key2 * 0.09 + top_strip * 0.038 + horizon
         } elif(kind == 1){
            c0, c1 = 0.70 + 0.15 * top_t - 0.08 * floor_t, 0.66 + 0.14 * top_t - 0.07 * floor_t
            c2 = 0.74 + 0.16 * top_t - 0.04 * floor_t
            def broad1_dx, broad1_dy = (u - 0.72) / 0.18, (v - 0.24) / 0.16
            def broad1 = exp(-(broad1_dx * broad1_dx + broad1_dy * broad1_dy))
            def broad2_dx = (u - 0.28) / 0.18
            def broad2_dy = (v - 0.30) / 0.18
            def broad2 = exp(-(broad2_dx * broad2_dx + broad2_dy * broad2_dy))
            def top_strip = exp(-pow((v - 0.16) / 0.040, 2.0)) * exp(-pow((u - 0.50) / 0.34, 6.0))
            def horizon = exp(-pow((v - 0.56) / 0.14, 2.0))
            def warm_floor = exp(-pow((v - 0.82) / 0.16, 2.0))
            c0 += broad1 * 0.16 + broad2 * 0.12 + top_strip * 0.08 + horizon * 0.06 + warm_floor * 0.05
            c1 += broad1 * 0.12 + broad2 * 0.10 + top_strip * 0.06 + horizon * 0.04 + warm_floor * 0.03
            c2 += broad1 * 0.18 + broad2 * 0.16 + top_strip * 0.10 + horizon * 0.08 + warm_floor * 0.05
         } elif(kind == 2){
            mut l = 0.30 + 0.42 * top_t + 0.02 * floor_t
            def key_left_dx, key_left_dy = (u - 0.22) / 0.090, (v - 0.23) / 0.085
            def key_left = exp(-(key_left_dx * key_left_dx + key_left_dy * key_left_dy))
            def key_right_dx = (u - 0.78) / 0.095
            def key_right_dy = (v - 0.24) / 0.090
            def key_right = exp(-(key_right_dx * key_right_dx + key_right_dy * key_right_dy))
            def top_strip = exp(-pow((v - 0.15) / 0.070, 2.0)) * exp(-pow((u - 0.50) / 0.42, 4.0))
            def mid_soft = exp(-pow((v - 0.44) / 0.16, 2.0)) * exp(-pow((u - 0.50) / 0.36, 4.0))
            def floor_soft = exp(-pow((v - 0.82) / 0.18, 2.0))
            def center_shadow_x = max(abs((u - 0.50) / 0.25), abs((v - 0.63) / 0.30))
            def center_shadow = exp(-pow(center_shadow_x, 4.0))
            l += key_left * 1.05 + key_right * 1.00 + top_strip * 0.24 + mid_soft * 0.10 + floor_soft * 0.035
            l -= center_shadow * 0.035
            l = max(l, 0.075)
            def warm = key_left * 0.035 + floor_t * 0.018
            def cool = key_right * 0.025 + top_t * 0.020
            c0, c1 = l * 1.01 + warm, l * 1.00
            c2 = l * 1.02 + cool
         } else {
            c0, c1 = 0.18 + 0.08 * sky_t - 0.02 * floor_t, 0.18 + 0.08 * sky_t - 0.02 * floor_t
            c2 = 0.20 + 0.09 * sky_t - 0.02 * floor_t
            def soft1_dx, soft1_dy = (u - 0.24) / 0.060, (v - 0.23) / 0.055
            def soft1 = exp(-(soft1_dx * soft1_dx + soft1_dy * soft1_dy))
            def soft2_dx = (u - 0.76) / 0.060
            def soft2_dy = (v - 0.23) / 0.055
            def soft2 = exp(-(soft2_dx * soft2_dx + soft2_dy * soft2_dy))
            def fill_dx = (u - 0.50) / 0.20
            def fill_dy = (v - 0.19) / 0.08
            def fill = exp(-(fill_dx * fill_dx + fill_dy * fill_dy))
            def warm_dx = (u - 0.50) / 0.30
            def warm_dy = (v - 0.74) / 0.16
            def warm = exp(-(warm_dx * warm_dx + warm_dy * warm_dy))
            def horizon = exp(-pow((v - 0.50) / 0.090, 2.0))
            c0 += soft1 * 1.14 + soft2 * 1.14 + fill * 0.30 + warm * 0.04 + horizon * 0.03
            c1 += soft1 * 1.14 + soft2 * 1.14 + fill * 0.30 + warm * 0.04 + horizon * 0.03
            c2 += soft1 * 1.16 + soft2 * 1.16 + fill * 0.31 + warm * 0.03 + horizon * 0.04
         }
         def p = row + x * 4
         store8(pixels, linear_to_srgb_u8(c0), p + 0)
         store8(pixels, linear_to_srgb_u8(c1), p + 1)
         store8(pixels, linear_to_srgb_u8(c2), p + 2)
         store8(pixels, 255, p + 3)
         x += 1
      }
      y += 1
   }
   _rgba_image_result(pixels, iw, ih)
}

fn generate_neutral_env_image(int: w=1024, int: h=512): any { generate_env_image(0, w, h) }

fn generate_compare_visible_env_image(int: w=1024, int: h=512): any { generate_env_image(1, w, h) }

fn generate_compare_reflect_env_image(int: w=1024, int: h=512): any { generate_env_image(2, w, h) }

fn generate_studio_env_image(int: w=1024, int: h=512): any { generate_env_image(3, w, h) }

fn _scene_studio_compare_exclude(): list {
   [
      "CompareEmissiveStrength", "CompareAlphaCoverage", "CompareTransmission", "CompareVolume",
      "CompareIor", "CompareDispersion", "CompareRoughness", "CompareNormal",
      "CompareClearcoat", "CompareSheen", "CompareSpecular", "CompareIridescence",
      "CompareAnisotropy", "CompareMetallic"
   ]
}

fn _scene_studio_grid_exclude(): list { ["SheenTestGrid", "TransmissionThinwallTestGrid", "IORTestGrid"] }

fn _scene_studio_names(): list {
   [
      "AnisotropyDiscTest", "AnisotropyRotationTest", "AnisotropyStrengthTest",
      "MetalRoughSpheres", "MetalRoughSpheresNoTextures", "SpecGlossVsMetalRough",
      "SheenChair", "ClearCoatTest", "SheenWoodLeatherSofa", "GlamVelvetSofa", "ToyCar"
   ]
}

fn _scene_neutral_names(): list {
   ["CompareAlphaCoverage", "LightVisibility", "CompareMetallic", "CompareTransmission", "CompareVolume", "CompareIor", "CompareDispersion", "CompareNormal", "CompareRoughness", "CompareClearcoat", "CompareSheen", "CompareSpecular", "CompareIridescence", "CompareAnisotropy", "SheenTestGrid", "MultiUVTest", "TextureLinearInterpolationTest", "TextureTransformTest", "TextureTransformMultiTest", "TextureEncodingTest", "DiffuseTransmissionTest", "DiffuseTransmissionPlant", "DiffuseTransmissionTeacup", "TransmissionOrderTest", "TransmissionRoughnessTest", "TransmissionThinwallTestGrid", "TransmissionTest", "IORTestGrid", "USDShaderBallForGltf", "AttenuationTest", "DragonAttenuation", "DispersionTest", "DragonDispersion", "DirectionalLight", "PointLightIntensityTest", "LightsPunctualLamp", "PlaysetLightTest", "EnvironmentTest", "EmissiveStrengthTest", "SpecularTest", "Lantern", "SheenChair", "ChronographWatch", "CommercialRefrigerator", "GlassHurricaneCandleHolder", "MosquitoInAmber", "GlassVaseFlowers", "ScatteringSkull", "IridescentDishWithOlives", "IridescenceSuzanne", "IridescenceLamp", "IridescenceBalloon", "SheenWoodLeatherSofa"]
}

fn _scene_reflect_names(): list {
   [
      "MetalRoughSpheres", "MetalRoughSpheresNoTextures", "SpecGlossVsMetalRough",
      "CompareRoughness", "CompareClearcoat", "CompareMetallic", "CompareIridescence",
      "CompareAnisotropy", "SpecularTest", "SheenChair", "CompareSheen", "CompareSpecular",
      "SheenTestGrid", "TextureTransformMultiTest", "TransmissionThinwallTestGrid",
      "EnvironmentTest", "IridescenceMetallicSpheres", "IridescentDishWithOlives",
      "IridescenceSuzanne", "IridescenceLamp", "IridescenceBalloon", "SheenWoodLeatherSofa",
      "GlamVelvetSofa", "ToyCar", "ClearCoatTest"
   ]
}

fn _scene_visible_names(): list {
   [
      "TransmissionThinwallTestGrid", "EnvironmentTest", "USDShaderBallForGltf",
      "GlassHurricaneCandleHolder", "MosquitoInAmber", "GlassVaseFlowers", "ScatteringSkull",
      "SunglassesKhronos", "TransmissionRoughnessTest", "TransmissionTest"
   ]
}

fn _scene_optical_names(): list {
   [
      "CompareTransmission", "CompareVolume", "CompareIor", "CompareDispersion",
      "DiffuseTransmissionTest", "DiffuseTransmissionPlant", "DiffuseTransmissionTeacup",
      "TransmissionOrderTest", "TransmissionRoughnessTest", "TransmissionThinwallTestGrid",
      "TransmissionTest", "IORTestGrid", "USDShaderBallForGltf", "AttenuationTest",
      "DragonAttenuation", "DispersionTest", "DragonDispersion", "SunglassesKhronos",
      "WaterBottle", "CommercialRefrigerator", "GlassHurricaneCandleHolder", "MosquitoInAmber",
      "GlassVaseFlowers", "ScatteringSkull", "IridescenceLamp", "IridescentDishWithOlives",
      "IridescenceSuzanne"
   ]
}

fn _scene_black_visible_names(): list { ["IridescentDishWithOlives"] }

fn _scene_gray_proof_names(): list {
   ["BoomBoxWithAxes", "DamagedHelmet", "MaterialsVariantsShoe", "CompareRoughness", "AntiqueCamera", "SciFiHelmet", "Avocado", "Lantern", "CarbonFibre", "GlamVelvetSofa", "NodePerformanceTest", "Box", "BoxInterleaved", "BoxTextured", "BoxTexturedNonPowerOfTwo", "DiffuseTransmissionTest", "DiffuseTransmissionPlant", "LightsPunctualLamp", "MetalRoughSpheresNoTextures", "SimpleTexture", "SpecularSilkPouf", "TransmissionThinwallTestGrid", "IridescenceSuzanne", "DragonAttenuation", "DragonDispersion", "IORTestGrid", "TransmissionRoughnessTest", "MosquitoInAmber", "GlassVaseFlowers", "GlassHurricaneCandleHolder", "BarramundiFish", "ChairDamaskPurplegold", "Corset", "FlightHelmet", "IridescenceLamp", "IridescentDishWithOlives", "MorphPrimitivesTest", "ScatteringSkull", "CompareIor", "ClearCoatTest", "MeshoptCubeTest", "AnisotropyDiscTest"]
}

fn _name_in_list(any: name, any: items): bool {
   if(!is_list(items)){ return false }
   def s = str.strip(to_str(name))
   if(s.len == 0){ return false }
   mut i = 0
   while(i < items.len){
      if(s == to_str(items.get(i, ""))){ return true }
      i += 1
   }
   false
}

fn scene_prefers_studio_env(any: name): bool {
   def s = str.strip(to_str(name))
   if(s.len == 0){ return false }
   if(str.startswith(s, "Compare")){ return !_name_in_list(s, _scene_studio_compare_exclude()) }
   if(str.endswith(s, "TestGrid")){ return !_name_in_list(s, _scene_studio_grid_exclude()) }
   if(str.endswith(s, "Spheres")){ return true }
   _name_in_list(s, _scene_studio_names())
}

fn scene_prefers_neutral_env(any: name): bool { _name_in_list(name, _scene_neutral_names()) }

fn scene_prefers_compare_reflect_env(any: name): bool { _name_in_list(name, _scene_reflect_names()) }

fn scene_prefers_compare_visible_env(any: name): bool { _name_in_list(name, _scene_visible_names()) }

fn scene_prefers_optical_spec_env(any: name): bool { _name_in_list(name, _scene_optical_names()) }

fn scene_prefers_black_visible_env(any: name): bool { _name_in_list(name, _scene_black_visible_names()) }

fn scene_prefers_gray_proof_bg(any: name): bool { _name_in_list(name, _scene_gray_proof_names()) }
