#version 450
#extension GL_EXT_nonuniform_qualifier : enable
layout(location=0) in vec3 vDir ;
layout(location=1) flat in uint vTexIndex ;
layout(set=0, binding=0) uniform sampler2D texSamplers[1024] ;
layout(location=0) out vec4 outColor ;
const float PI = 3.141592653589793 ;
vec3 linearToSrgb(vec3 c){ return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)) ; }
vec3 neutralToneMap(vec3 color){
   const float startCompression = 0.8 - 0.04 ;
   const float desaturation = 0.15 ;
   float x = min(color.r, min(color.g, color.b)) ;
   float offset = x < 0.08 ? x - 6.25 * x * x : 0.04 ;
   color -= offset ;
   float peak = max(color.r, max(color.g, color.b)) ;
   if(peak < startCompression){ return max(color, vec3(0.0)) ; }
   const float d = 1.0 - startCompression ;
   float newPeak = 1.0 - d * d / (peak + d - startCompression) ;
   color *= newPeak / peak ;
   float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0) ;
   return mix(color, vec3(newPeak), g) ;
}
vec2 dirToLatLongUvRaw(vec3 d){
   d = normalize(d) ;
   float u = 0.5 + atan(d.x, 0.0 - d.z) / (2.0 * PI) ;
   float v = 0.5 - asin(clamp(d.y, -1.0, 1.0)) / PI ;
   return vec2(u, v) ;
}
vec3 sampleLatLongSeamLod(uint skyIndex, vec3 d, float lod){
   vec2 uvRaw = dirToLatLongUvRaw(d) ;
   float u = fract(uvRaw.x) ;
   float v = clamp(uvRaw.y, 1e-5, 0.99999) ;
   vec3 c = textureLod(texSamplers[nonuniformEXT(skyIndex)], vec2(u, v), lod).rgb ;
   ivec2 ts = textureSize(texSamplers[nonuniformEXT(skyIndex)], 0) ;
   float seamW = max(2.5 / float(max(ts.x, 1)), 1e-5) ;
   float seamDist = min(u, 1.0 - u) ;
   if(seamDist < seamW){
      float uAlt = (u < 0.5) ? (u + 1.0) : (u - 1.0) ;
      vec3 cAlt = textureLod(texSamplers[nonuniformEXT(skyIndex)], vec2(uAlt, v), lod).rgb ;
      float t = clamp(seamDist / seamW, 0.0, 1.0) ;
      c = mix((c + cAlt) * 0.5, c, t) ;
   }
   return c ;
}
void main(){
   uint skyIndex = vTexIndex ;
   if(skyIndex >= 1024u){
      outColor = vec4(0.05, 0.05, 0.08, 1.0) ;
      return ;
   }
   vec3 sky = neutralToneMap(sampleLatLongSeamLod(skyIndex, vDir, 0.0)) ;
   outColor = vec4(linearToSrgb(sky), 1.0) ;
}
