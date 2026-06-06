#version 450
#extension GL_EXT_nonuniform_qualifier : enable
layout(location=0) in vec4 vColor ;
layout(location=1) in vec2 vUV ;
layout(location=2) in vec3 vNormal ;
layout(location=3) flat in uint vTexIndex ;
layout(location=4) in vec3 vWorldPos ;
layout(location=5) in vec4 vTangent ;
layout(location=6) in vec2 vUV2 ;
layout(push_constant) uniform PC { mat4 vp ; mat4 model; int isMask; int isUnlit; uint baseColor; uint material; int viewportW; int viewportH; uint bsdf4Packed; uint bsdf5Packed; layout(offset=160) vec3 camPos; layout(offset=172) uint emissivePacked; layout(offset=176) uint emissiveTexIndex; layout(offset=180) uint baseTexIndex; layout(offset=184) uint alphaPacked; layout(offset=188) uint occlusionTexIndex; layout(offset=192) uint bsdf0Packed; layout(offset=196) uint bsdf1Packed; layout(offset=200) uint bsdf2Packed; layout(offset=204) uint bsdf3Packed; layout(offset=208) uint baseUvXf0; layout(offset=212) uint baseUvXf1; layout(offset=216) uint normalUvXf0; layout(offset=220) uint normalUvXf1; layout(offset=224) uint mrUvXf0; layout(offset=228) uint mrUvXf1; layout(offset=232) uint occlusionUvXf0; layout(offset=236) uint occlusionUvXf1; layout(offset=240) uint emissiveUvXf0; layout(offset=244) uint emissiveUvXf1; layout(offset=248) uint normalTexIndex; layout(offset=252) uint mrTexIndex; } pc;
layout(set=0, binding=0) uniform sampler2D texSamplers[1024] ;
layout(set=1, binding=0, std140) uniform SceneBlock {
  vec4 lightPosType[8] ;
  vec4 lightColorRange[8] ;
  vec4 lightDirOuter[8] ;
} scene ;
layout(location=0) out vec4 outColor ;
const float PI = 3.141592653589793 ;
vec3 linearToSrgb(vec3 c){ return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)) ; }
vec3 srgbToLinear(vec3 c){
  bvec3 cutoff = lessThanEqual(c, vec3(0.04045)) ;
  vec3 lo = c / 12.92 ;
  vec3 hi = pow((c + vec3(0.055)) / 1.055, vec3(2.4)) ;
  return mix(hi, lo, cutoff) ;
}
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
float bayer4x4(vec2 fragXY){
  ivec2 p = ivec2(mod(floor(fragXY), 4.0)) ;
  int idx = p.x + p.y * 4 ;
  const float table[16] = float[16](
    0.0, 8.0, 2.0, 10.0,
    12.0, 4.0, 14.0, 6.0,
    3.0, 11.0, 1.0, 9.0,
    15.0, 7.0, 13.0, 5.0
  ) ;
  return (table[idx] + 0.5) / 16.0 ;
}
vec3 unpackEmissive(uint p){ float s = float((p >> 24u) & 255u) * (64.0 / 255.0) ; if(s <= 0.0) return vec3(0.0); return vec3(float(p & 255u), float((p >> 8u) & 255u), float((p >> 16u) & 255u)) * (s / 255.0); }
vec4 unpackColor(uint p){ return vec4(float(p & 255u), float((p >> 8u) & 255u), float((p >> 16u) & 255u), float((p >> 24u) & 255u)) / 255.0 ; }
vec4 resolveTint(vec4 vertexTint, vec4 materialTint){ return vertexTint * materialTint ; }
float decodeUvOffset16(uint q){ return mix(-8.0, 8.0, float(q) / 65535.0) ; }
float decodeUvScale11(uint q){
  uint v = q & 2047u ;
  return v == 0u ? 1.0 : (float(v) / 2047.0) * 64.0 - 32.0;
}
float uvScaleMagnitude(uint xf1){
  if((xf1 & 0x3fffffffu) == 0u){ return 1.0 ; }
  return max(abs(decodeUvScale11(xf1 & 2047u)), abs(decodeUvScale11((xf1 >> 11u) & 2047u))) ;
}
float normalTileAA(uint xf1){
  return smoothstep(8.0, 24.0, uvScaleMagnitude(xf1)) ;
}
float decodeUvRot8(uint q){ return (float(q & 255u) / 255.0) * (2.0 * PI) - PI ; }
float decodeAnisoRot7(uint q){ return (q == 64u) ? 0.0 : ((float(q & 127u) / 127.0) * (2.0 * PI) - PI) ; }
vec2 applyUvXf(vec2 uv, uint xf0, uint xf1){
  if(xf0 == 0u && (xf1 & 0x3fffffffu) == 0u){ return uv ; }
  vec2 off = vec2(decodeUvOffset16(xf0 & 65535u), decodeUvOffset16((xf0 >> 16u) & 65535u)) ;
  vec2 scl = vec2(decodeUvScale11(xf1 & 2047u), decodeUvScale11((xf1 >> 11u) & 2047u)) ;
  uint rotBits = (xf1 >> 22u) & 255u ;
  float rot = (rotBits == 128u) ? 0.0 : decodeUvRot8(rotBits) ;
  vec2 suv = uv * scl ;
  float cr = cos(rot) ;
  float sr = sin(rot) ;
  return vec2(cr * suv.x + sr * suv.y, -sr * suv.x + cr * suv.y) + off ;
}
vec2 baseUv(){ return applyUvXf(((pc.baseUvXf1 >> 30u) != 0u) ? vUV2 : vUV, pc.baseUvXf0, pc.baseUvXf1) ; }
vec2 mapUv(uint packedIndex, uint xf0, uint xf1){ return applyUvXf(((packedIndex & 0x10000u) != 0u) ? vUV2 : vUV, xf0, xf1) ; }
float modelThicknessScale(){
  mat3 m = mat3(pc.model) ;
  float detAbs = abs(determinant(m)) ;
  if(detAbs > 1e-8){ return pow(detAbs, 1.0 / 3.0) ; }
  float sx = length(m[0]) ;
  float sy = length(m[1]) ;
  float sz = length(m[2]) ;
  return max((sx + sy + sz) / 3.0, 1e-4) ;
}
vec3 geometricNormal(){
  vec3 dp1 = dFdx(vWorldPos) ;
  vec3 dp2 = dFdy(vWorldPos) ;
  vec3 g = cross(dp1, dp2) ;
  float gl = length(g) ;
  if(gl < 1e-5){ return vec3(0.0, 0.0, 1.0) ; }
  return g / gl ;
}
vec3 getNormal(){
  float nl = length(vNormal) ;
  if(nl < 1e-5){ return geometricNormal() ; }
  return vNormal / nl;
}
float decodeNormalScale7(uint packedIndex){
  return float((packedIndex >> 24u) & 127u) / 127.0 * 2.0 ;
}
vec3 applyNormalMap(vec3 N, vec4 authoredTangent, uint packedIndex, uint xf0, uint xf1, float normalScale){
  uint normalIndex = packedIndex & 0xFFFFu ;
  if(normalIndex >= 1024u){ return N ; }
  vec2 uv = mapUv(packedIndex, xf0, xf1) ;
  vec3 T = authoredTangent.xyz ;
  float handedness = authoredTangent.w ;
  if(length(T) < 1e-5){
    vec3 dp1 = dFdx(vWorldPos) ;
    vec3 dp2 = dFdy(vWorldPos) ;
    vec2 duv1 = dFdx(uv) ;
    vec2 duv2 = dFdy(uv) ;
    T = dp1 * duv2.y - dp2 * duv1.y ;
    vec3 Btest = -dp1 * duv2.x + dp2 * duv1.x ;
    if(length(T) < 1e-5 || length(Btest) < 1e-5){ return N ; }
    float uvDet = duv1.x * duv2.y - duv1.y * duv2.x ;
    handedness = uvDet < 0.0 ? -1.0 : 1.0 ;
  }
  bool mirroredDoubleSided = (packedIndex & 0x20000u) != 0u ;
  bool logicalBackface = mirroredDoubleSided ? false : !gl_FrontFacing ;
  if(logicalBackface){ handedness = -handedness ; }
  T = normalize(T - N * dot(N, T)) ;
  vec3 B = normalize(cross(N, T)) * handedness ;
  normalScale *= mix(1.0, 0.20, normalTileAA(xf1)) ;
  vec3 mapN = texture(texSamplers[nonuniformEXT(normalIndex)], uv).xyz * 2.0 - 1.0 ;
  mapN.xy *= normalScale ;
  return normalize(mat3(T, B, N) * mapN) ;
}
float sat1(float x){ return clamp(x, 0.0, 1.0) ; }
vec3 sat3(vec3 x){ return clamp(x, vec3(0.0), vec3(1.0)) ; }
float pow5(float x){ x = sat1(x) ; float x2 = x * x ; return x2 * x2 * x ; }
float max3(vec3 v){ return max(v.r, max(v.g, v.b)) ; }
float perceivedBrightness(vec3 c){ return sqrt(max(0.0, 0.299 * c.r * c.r + 0.587 * c.g * c.g + 0.114 * c.b * c.b)) ; }
float solveSpecGlossMetallic(vec3 diffuse, vec3 specular, float oneMinusSpecularStrength){
  const float dielectricSpecular = 0.04 ;
  float specularBrightness = perceivedBrightness(specular) ;
  if(specularBrightness < dielectricSpecular){ return 0.0 ; }
  float diffuseBrightness = perceivedBrightness(diffuse) ;
  float a = dielectricSpecular ;
  float b = diffuseBrightness * oneMinusSpecularStrength / (1.0 - dielectricSpecular) + specularBrightness - 2.0 * dielectricSpecular ;
  float c = dielectricSpecular - specularBrightness ;
  float d = max(b * b - 4.0 * a * c, 0.0) ;
  return clamp((-b + sqrt(d)) / (2.0 * a), 0.0, 1.0) ;
}
vec3 F_Schlick(vec3 f0, float VoH){
  float f = pow5(1.0 - VoH) ;
  return f0 + (vec3(1.0) - f0) * f ;
}
vec3 F_Rough(vec3 f0, float NoV, float r){
  return f0 + (max(vec3(1.0 - r), f0) - f0) * pow5(1.0 - NoV) ;
}
vec3 materialFresnel(vec3 dielectricF0, float dielectricF90, vec3 metalF0, float metallic, float cosTheta){
  vec3 fd = dielectricF0 + (vec3(dielectricF90) - dielectricF0) * pow5(1.0 - cosTheta) ;
  vec3 fm = F_Schlick(metalF0, cosTheta) ;
  return mix(fd, fm, sat1(metallic)) ;
}
float D_GGX(float NoH, float r){
  float a = max(r * r, 0.002) ;
  float a2 = a * a ;
  float d = NoH * NoH * (a2 - 1.0) + 1.0 ;
  return a2 / max(PI * d * d, 1e-5) ;
}
float V_Smith(float NoV, float NoL, float r){
  float a = max(r * r, 0.002) ;
  float gv = NoL * sqrt(max(NoV * (NoV - NoV * a) + a, 0.0)) ;
  float gl = NoV * sqrt(max(NoL * (NoL - NoL * a) + a, 0.0)) ;
  return 0.5 / max(gv + gl, 1e-5) ;
}
float D_GGX_Aniso(vec3 H, vec3 N, vec3 T, vec3 B, float r, float an){
  float alphaRoughness = max(r * r, 0.002) ;
  float aniso = min(sat1(abs(an)), 0.99) ;
  float at = max(mix(alphaRoughness, 1.0, aniso * aniso), 0.002) ;
  float ab = max(alphaRoughness, 0.002) ;
  float NoH = max(dot(N, H), 0.0) ;
  float ToH = dot(T, H) ;
  float BoH = dot(B, H) ;
  float a2 = max(at * ab, 1e-5) ;
  vec3 f = vec3(ab * ToH, at * BoH, a2 * NoH) ;
  float w2 = a2 / max(dot(f, f), 1e-7) ;
  return min(a2 * w2 * w2 / PI, 64.0) ;
}
float V_GGX_Aniso(vec3 V, vec3 L, vec3 N, vec3 T, vec3 B, float r, float an){
  float alphaRoughness = max(r * r, 0.002) ;
  float aniso = min(sat1(abs(an)), 0.99) ;
  float at = max(mix(alphaRoughness, 1.0, aniso * aniso), 0.002) ;
  float ab = max(alphaRoughness, 0.002) ;
  float NoL = max(dot(N, L), 0.001) ;
  float NoV = max(dot(N, V), 0.001) ;
  float ToV = dot(T, V) ;
  float BoV = dot(B, V) ;
  float ToL = dot(T, L) ;
  float BoL = dot(B, L) ;
  float ggxV = NoL * length(vec3(at * ToV, ab * BoV, NoV)) ;
  float ggxL = NoV * length(vec3(at * ToL, ab * BoL, NoL)) ;
  return clamp(0.5 / max(ggxV + ggxL, 1e-5), 0.0, 1.0) ;
}
float D_Charlie(float NoH, float sheenRoughness){
  float ag = max(sheenRoughness * sheenRoughness, 0.02) ;
  float invR = 1.0 / ag ;
  float sin2h = max(1.0 - NoH * NoH, 0.0) ;
  return (2.0 + invR) * pow(sin2h, invR * 0.5) / (2.0 * PI) ;
}
float V_Sheen(float NoV, float NoL){
  return 1.0 / max(4.0 * (NoL + NoV - NoL * NoV), 0.001) ;
}
vec2 dirToLatLongUv(vec3 d){
  d = normalize(d) ;
  float u = 0.5 + atan(d.x, 0.0 - d.z) / (2.0 * PI) ;
  float v = 0.5 - asin(clamp(d.y, -1.0, 1.0)) / PI ;
  return vec2(fract(u), clamp(v, 1e-5, 0.99999)) ;
}
vec2 dirToLatLongUvRaw(vec3 d){
  d = normalize(d) ;
  float u = 0.5 + atan(d.x, 0.0 - d.z) / (2.0 * PI) ;
  float v = 0.5 - asin(clamp(d.y, -1.0, 1.0)) / PI ;
  return vec2(u, v) ;
}
vec3 sampleLatLongSeamLod(uint texIndex, vec3 d, float lod){
  vec2 uvRaw = dirToLatLongUvRaw(d) ;
  float u = fract(uvRaw.x) ;
  float v = clamp(uvRaw.y, 1e-5, 0.99999) ;
  vec3 c = textureLod(texSamplers[nonuniformEXT(texIndex)], vec2(u, v), lod).rgb ;
  ivec2 ts = textureSize(texSamplers[nonuniformEXT(texIndex)], 0) ;
  float seamW = max(2.5 / float(max(ts.x, 1)), 1e-5) ;
  float seamDist = min(u, 1.0 - u) ;
  if(seamDist < seamW){
    float uAlt = (u < 0.5) ? (u + 1.0) : (u - 1.0) ;
    vec3 cAlt = textureLod(texSamplers[nonuniformEXT(texIndex)], vec2(uAlt, v), lod).rgb ;
    float t = clamp(seamDist / seamW, 0.0, 1.0) ;
    c = mix((c + cAlt) * 0.5, c, t) ;
  }
  return c ;
}
bool hasSceneColorCapture(){
  return (uint(pc.viewportW) & 0x80000000u) != 0u ;
}
int sceneColorTexIndex(){
  if(!hasSceneColorCapture()){ return -1 ; }
  uint packed = uint(pc.viewportW) ;
  uint word = packed & 0x7ffu ;
  return word == 0u ? -1 : int(word - 1u) ;
}
int envDiffuseTexIndex(){
  if(!hasSceneColorCapture()){ return pc.viewportW ; }
  uint packed = uint(pc.viewportW) ;
  uint word = (packed >> 11u) & 0x7ffu ;
  return word == 0u ? -1 : int(word - 1u) ;
}
vec2 sceneColorUv(vec2 pxOffset){
  int sceneIndex = sceneColorTexIndex() ;
  if(sceneIndex < 0 || sceneIndex >= 1024){ return vec2(0.5) ; }
  ivec2 sz = textureSize(texSamplers[nonuniformEXT(uint(sceneIndex))], 0) ;
  vec2 sizef = vec2(max(sz.x, 1), max(sz.y, 1)) ;
  vec2 uv = (gl_FragCoord.xy + pxOffset) / sizef ;
  return clamp(uv, vec2(0.0), vec2(1.0)) ;
}
vec3 sampleSceneColor(vec2 pxOffset){
  int sceneIndex = sceneColorTexIndex() ;
  if(sceneIndex < 0 || sceneIndex >= 1024){ return vec3(0.0) ; }
  return srgbToLinear(textureLod(texSamplers[nonuniformEXT(uint(sceneIndex))], sceneColorUv(pxOffset), 0.0).rgb) ;
}
vec3 fallbackEnvDiffuse(vec3 d){
  vec3 skyCol = vec3(0.78, 0.86, 0.98) ;
  vec3 zenithCol = vec3(0.08, 0.16, 0.30) ;
  vec3 groundCol = vec3(0.10, 0.08, 0.07) ;
  float nSky = clamp(d.y * 0.5 + 0.5, 0.0, 1.0) ;
  return mix(groundCol, mix(skyCol, zenithCol, clamp(d.y, 0.0, 1.0)), nSky) ;
}
vec3 fallbackEnvSpec(vec3 d, float roughness){
  vec3 skyCol = vec3(0.78, 0.86, 0.98) ;
  vec3 zenithCol = vec3(0.08, 0.16, 0.30) ;
  vec3 groundCol = vec3(0.10, 0.08, 0.07) ;
  vec3 horizonCol = vec3(0.92, 0.72, 0.56) ;
  float rSky = clamp(d.y * 0.5 + 0.5, 0.0, 1.0) ;
  float rHorizon = 1.0 - abs(d.y) ;
  float rSkySharp = pow(rSky, 3.2) ;
  vec3 envSpec = mix(groundCol * 0.02, mix(skyCol * 0.82, zenithCol * 0.42, clamp(d.y, 0.0, 1.0)), rSkySharp) ;
  envSpec += horizonCol * pow(clamp(rHorizon, 0.0, 1.0), 16.0) * mix(0.18, 0.48, 1.0 - roughness) ;
  vec3 sunDirEnv = normalize(vec3(-0.38, 0.78, 0.22)) ;
  float sunSpot = pow(max(dot(d, sunDirEnv), 0.0), mix(20.0, 650.0, 1.0 - roughness)) ;
  envSpec += vec3(1.18, 1.00, 0.84) * sunSpot * mix(0.18, 3.20, 1.0 - roughness) ;
  return envSpec ;
}
vec3 opticalLightRigEnv(vec3 d, float roughness){
  vec2 uv = dirToLatLongUv(d) ;
  float horizon = exp(-pow(d.y / 0.34, 2.0)) ;
  float upper = clamp(d.y * 0.5 + 0.5, 0.0, 1.0) ;
  vec3 base = mix(vec3(0.018, 0.020, 0.022), vec3(0.22, 0.28, 0.34), upper) ;
  float warmWall = exp(-pow((uv.x - 0.21) / 0.055, 2.0)) * exp(-pow((uv.y - 0.52) / 0.34, 2.0)) ;
  float coolWall = exp(-pow((uv.x - 0.48) / 0.050, 2.0)) * exp(-pow((uv.y - 0.50) / 0.32, 2.0)) ;
  float greenWall = exp(-pow((uv.x - 0.70) / 0.060, 2.0)) * exp(-pow((uv.y - 0.48) / 0.30, 2.0)) ;
  float softBox = exp(-pow((uv.x - 0.86) / 0.085, 2.0)) * exp(-pow((uv.y - 0.46) / 0.18, 2.0)) ;
  float floorBand = exp(-pow((uv.y - 0.63) / 0.060, 2.0)) ;
  vec3 bands = vec3(0.0) ;
  bands += vec3(1.30, 0.58, 0.20) * warmWall ;
  bands += vec3(0.32, 0.70, 1.22) * coolWall ;
  bands += vec3(0.18, 1.06, 0.72) * greenWall ;
  bands += vec3(1.65, 1.48, 1.18) * softBox ;
  bands += vec3(1.18, 0.92, 0.55) * floorBand * horizon ;
  float gloss = mix(1.0, 0.28, clamp(roughness, 0.0, 1.0)) ;
  return base + bands * horizon * gloss ;
}
vec3 sampleEnvDiffuse(vec3 d){
  int envIndex = envDiffuseTexIndex() ;
  if(envIndex >= 0 && envIndex < 1024){
    return srgbToLinear(sampleLatLongSeamLod(uint(envIndex), d, 6.0)) ;
  }
  return fallbackEnvDiffuse(d) ;
}
vec3 sampleEnvSpec(vec3 d, float roughness){
  int envIndex = pc.viewportH ;
  if(envIndex < 0 || envIndex >= 1024){
    envIndex = envDiffuseTexIndex() ;
  }
  if(envIndex >= 0 && envIndex < 1024){
    float lod = clamp(roughness * roughness * 6.0, 0.0, 6.0) ;
    return srgbToLinear(sampleLatLongSeamLod(uint(envIndex), d, lod)) ;
  }
  return fallbackEnvSpec(d, roughness) ;
}
vec3 envBrdfApprox(vec3 f0, vec3 f90, float roughness, float NoV){
  vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022) ;
  vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04) ;
  vec4 r = roughness * c0 + c1 ;
  float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y ;
  vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw ;
  return max(f0 * ab.x + f90 * ab.y, vec3(0.0)) ;
}
void main(){
  bool vertexColorPrimary = (pc.baseTexIndex & 0x40000000u) != 0u ;
  bool vertexColorMultiply = (pc.baseTexIndex & 0x10000000u) != 0u ;
  bool specGlossWorkflow = (pc.baseTexIndex & 0x20000000u) != 0u ;
  bool sourceSpecGlossWorkflow = specGlossWorkflow ;
  bool vertexTextureIndex = (pc.baseTexIndex & 0x04000000u) != 0u ;
  bool vertexPackedMaterial = (pc.baseTexIndex & 0x08000000u) != 0u ;
  vec4 baseTint = resolveTint((vertexColorPrimary || vertexColorMultiply) ? vColor : vec4(1.0), unpackColor(pc.baseColor)) ;
  uint baseIndex = pc.baseTexIndex ;
  if((baseIndex & 0x80000000u) != 0u){
    // UI/font batches carry atlas pages per vertex; glTF meshes with no
    // authored base texture leave this slot at zero and should stay white.
    uint vertexIndex = vTexIndex & 0xFFFFu ;
    // UI/font batches can legitimately use texture slot 0. Sampling the default
    // white texture is also the correct fallback for untextured vertex-color UI.
    baseIndex = (vertexTextureIndex && vertexIndex < 1024u) ? vertexIndex : 0xFFFFFFFFu ;
  } else {
    baseIndex = baseIndex & 0xFFFFu ;
  }

  vec4 tex = vec4(1.0) ;
  vec2 baseUV = baseUv() ;
  if(!vertexColorPrimary && baseIndex < 1024u){ tex = texture(texSamplers[nonuniformEXT(baseIndex)], baseUV) ; }
  vec3 texBaseLinear = tex.rgb ;

  uint alphaMode = pc.alphaPacked & 3u ;
  float alphaCutoff = float((pc.alphaPacked >> 8u) & 255u) / 255.0 ;
  float occStrength = float((pc.alphaPacked >> 16u) & 255u) / 255.0 ;
  bool extSpecularMap = (pc.alphaPacked & 0x01000000u) != 0u ;
  bool extSpecularColorMap = (pc.alphaPacked & 0x02000000u) != 0u ;
  bool extThicknessMap = (pc.alphaPacked & 0x04000000u) != 0u ;
  bool extIridescenceMap = (pc.alphaPacked & 0x08000000u) != 0u ;
  bool extSpecularHDR = (pc.alphaPacked & 0x10000000u) != 0u ;
  bool extClearcoatMap = (pc.alphaPacked & 0x20000000u) != 0u ;
  bool extSheenColorMap = (pc.alphaPacked & 0x40000000u) != 0u ;
  bool extDiffuseTransmissionColor = (pc.alphaPacked & 0x80000000u) != 0u ;
  // Debug views must not alias live extension transport bits.
  bool dbgSceneLights = false ;
  bool dbgBaseColor = false ;
  bool dbgEmissive = false ;
  if(dbgSceneLights){
    vec3 dbg = vec3(0.0) ;
    for(int li = 0 ; li < 8; li++){
      vec4 lPosType  = scene.lightPosType[li] ;
      vec4 lColRange = scene.lightColorRange[li] ;
      vec3 lColor = lColRange.rgb ;
      if(dot(lColor, lColor) < 0.0001){ continue ; }
      float lType = lPosType.w ;
      if(lType < 0.5){ continue ; }
      vec3 toLight = lPosType.xyz - vWorldPos ;
      float dist2 = max(dot(toLight, toLight), 0.0001) ;
      float atten = 1.0 / (1.0 + dist2 * 0.08) ;
      dbg += lColor * atten ;
    }
    float peak = max(max(dbg.r, dbg.g), max(dbg.b, 1.0)) ;
    outColor = vec4(linearToSrgb(clamp(dbg / peak, 0.0, 1.0)), 1.0) ;
    return ;
  }
  float alphaCoverageRaw = float((pc.bsdf5Packed >> 24u) & 255u) / 255.0 ;
  float alphaCoverage = alphaCoverageRaw <= 0.0 ? 1.0 : alphaCoverageRaw ;
  bool implicitFlatUnlit = false ;
  if(pc.isUnlit != 0 || implicitFlatUnlit){
    float rawAlpha = tex.a * baseTint.a ;
    bool vertexCoverageMask = alphaMode == 2u && vertexColorMultiply && baseIndex < 1024u ;
    if(vertexCoverageMask){
      float rgbCoverage = max(max(tex.r, tex.g), tex.b) ;
      float coverage = max(tex.a, rgbCoverage) ;
      rawAlpha = coverage * baseTint.a ;
    }
    float baseAlpha = rawAlpha ;
    // No real alpha-to-coverage pipeline is wired yet, so emulate coverage with
    // stable ordered dithering instead of silently ignoring the extension.
    if(alphaCoverage < 0.999 && alphaMode == 2u){
      if(rawAlpha * alphaCoverage <= bayer4x4(gl_FragCoord.xy)){ discard ; }
      baseAlpha = 1.0 ;
    }

	  if(alphaMode == 1u){
	    if(rawAlpha < alphaCutoff){ discard ; }
	    baseAlpha = 1.0 ;
	  } else if(alphaMode == 2u){
	    if(baseAlpha <= 0.001){ discard ; }
	  }
	  bool gltfCullMaterial = (pc.normalTexIndex & 0x80000u) != 0u ;
	  bool doubleSidedMaterial = (pc.normalTexIndex & 0x40000u) != 0u ;
	  bool mirroredFacingUnlit = (pc.normalTexIndex & 0x20000u) != 0u ;
	  bool logicalBackfaceUnlit = mirroredFacingUnlit ? false : !gl_FrontFacing ;
	  if(gltfCullMaterial && !doubleSidedMaterial && !gl_FrontFacing){ discard ; }

	  vec3 unlitRgb = vertexCoverageMask ? baseTint.rgb : texBaseLinear * baseTint.rgb ;
	  vec3 baseRgb = (vertexColorPrimary || vertexCoverageMask) ? unlitRgb : linearToSrgb(unlitRgb) ;
	  outColor = vec4(baseRgb, baseAlpha) ;
	  return ;
  }

  float rawAlpha = tex.a * baseTint.a ;
  bool vertexCoverageMask = alphaMode == 2u && vertexColorMultiply && baseIndex < 1024u ;
  if(vertexCoverageMask){
    float rgbCoverage = max(max(tex.r, tex.g), tex.b) ;
    float coverage = max(tex.a, rgbCoverage) ;
    rawAlpha = coverage * baseTint.a ;
    texBaseLinear = baseTint.rgb ;
    baseTint = vec4(1.0) ;
  }
  if(alphaCoverage < 0.999 && alphaMode == 2u){
    if(rawAlpha * alphaCoverage <= bayer4x4(gl_FragCoord.xy)){ discard ; }
  }
  if(alphaMode == 2u){
    rawAlpha = rawAlpha ;
  }
  float baseAlpha = alphaMode == 2u ? clamp(rawAlpha, 0.0, 1.0) : 1.0 ;

  if(alphaMode == 1u){
    if(rawAlpha < alphaCutoff){ discard ; }
    baseAlpha = 1.0 ;
  } else if(alphaMode == 2u){
    if(baseAlpha <= 0.001){ discard ; }
  }

	  bool gltfCullMaterial = (pc.normalTexIndex & 0x80000u) != 0u ;
	  bool doubleSidedMaterial = (pc.normalTexIndex & 0x40000u) != 0u ;
	  bool mirroredFacing = (pc.normalTexIndex & 0x20000u) != 0u ;
	  bool logicalBackface = mirroredFacing ? false : !gl_FrontFacing ;
	  if(gltfCullMaterial && !doubleSidedMaterial && !gl_FrontFacing){ discard ; }
	  vec3 N = getNormal() ;
	  vec3 V = normalize(pc.camPos - vWorldPos + vec3(0.00001)) ;
	  if(logicalBackface){ N = -N ; }
  bool clearcoatOnlyNormal = (pc.normalTexIndex & 0x80000000u) != 0u ;
  float normalScale = decodeNormalScale7(pc.normalTexIndex) ;
  if(normalScale <= 0.0001){ normalScale = 1.0 ; }
  float normalMapTileAA = ((pc.normalTexIndex & 0xFFFFu) < 1024u) ? normalTileAA(pc.normalUvXf1) * clamp(normalScale / 0.30, 0.0, 1.0) : 0.0 ;
  vec3 coatNormal = N ;
  vec3 mappedNormal = applyNormalMap(N, vTangent, pc.normalTexIndex, pc.normalUvXf0, pc.normalUvXf1, normalScale) ;
  if(clearcoatOnlyNormal){
     coatNormal = mappedNormal ;
  } else {
     N = mappedNormal ;
     coatNormal = N ;
  }
  float normalMapVariance = 0.0 ;
  if((pc.normalTexIndex & 0xFFFFu) < 1024u){
     vec3 ndx = dFdx(N) ;
     vec3 ndy = dFdy(N) ;
     normalMapVariance = clamp((dot(ndx, ndx) + dot(ndy, ndy)) * 0.18, 0.0, 1.0) ;
     normalMapVariance = max(normalMapVariance, normalMapTileAA) ;
  }
  float metallic = vertexPackedMaterial ? vColor.r : float(pc.material & 255u) / 255.0 ;
  float roughnessRaw = vertexPackedMaterial ? vColor.g : float((pc.material >> 8u) & 255u) / 255.0 ;
  float roughness = max(roughnessRaw, 0.04) ;

  uint mrPacked = vertexPackedMaterial ? 0u : (pc.material >> 16u) ;
  uint mrWord = mrPacked & 0x7FFFu ;
  bool hasMrTexture = mrWord != 0u ;
  uint mrIndex = hasMrTexture ? (mrWord - 1u) : 0xFFFFFFFFu ;
  vec2 mrUV = applyUvXf(((mrPacked & 0x8000u) != 0u) ? vUV2 : vUV, pc.mrUvXf0, pc.mrUvXf1) ;
  vec4 mrTex = vec4(1.0) ;
  vec3 specGlossTexColor = vec3(1.0) ;
  bool mrSampled = false ;
  if(hasMrTexture && mrIndex < 1024u){
    mrTex = texture(texSamplers[nonuniformEXT(mrIndex)], mrUV) ;
    specGlossTexColor = srgbToLinear(mrTex.rgb) ;
    mrSampled = true ;
    if(specGlossWorkflow){
      float glossinessFactor = 1.0 - roughnessRaw ;
      float glossiness = clamp(glossinessFactor * mrTex.a, 0.0, 1.0) ;
      roughness = max(1.0 - glossiness, 0.04) ;
      metallic = 0.0 ;
    } else {
      roughness = max(roughness * mrTex.g, 0.04) ;
      float metallicTex = mrTex.b ;
      metallicTex = metallicTex < 0.08 ? 0.0 : (metallicTex > 0.72 ? min(1.0, metallicTex * 1.18) : metallicTex) ;
      metallic = clamp(metallic * metallicTex, 0.0, 1.0) ;
    }
  }

  float specularFactor = float(pc.bsdf0Packed & 255u) / 255.0 ;
  float sheenRoughness = float((pc.bsdf0Packed >> 8u) & 255u) / 255.0 ;
  float transmission = float(pc.bsdf0Packed >> 16u & 255u) / 255.0 ;
  float iridescence = float(pc.bsdf0Packed >> 24u & 255u) / 255.0 ;

  vec3 specularColor = vec3(float(pc.bsdf1Packed & 255u), float((pc.bsdf1Packed >> 8u) & 255u), float((pc.bsdf1Packed >> 16u) & 255u)) / 255.0 ;
  float ior = 1.0 + float((pc.bsdf1Packed >> 24u) & 255u) / 255.0 * 1.5 ;
  if(specGlossWorkflow && mrSampled){
    specularColor *= specGlossTexColor ;
  }

  vec3 sheenColor = vec3(float(pc.bsdf2Packed & 255u), float((pc.bsdf2Packed >> 8u) & 255u), float((pc.bsdf2Packed >> 16u) & 255u)) / 255.0 ;
  float thickness = (float((pc.bsdf2Packed >> 24u) & 255u) / 255.0) * 4.0 ;

  vec3 attenuationColor = vec3(float(pc.bsdf3Packed & 255u), float((pc.bsdf3Packed >> 8u) & 255u), float((pc.bsdf3Packed >> 16u) & 255u)) / 255.0 ;
  uint bsdf3A = (pc.bsdf3Packed >> 24u) & 255u ;
  bool packedIriParams = iridescence > 0.001 && !extSpecularHDR && bsdf3A == 254u ;
  float attenuationDist = bsdf3A >= 254u ? 1.0 : float(bsdf3A) / 253.0 ;
  float specularHdrScale = extSpecularHDR ? mix(1.0, 16.0, float(bsdf3A) / 255.0) : 1.0 ;
  specularColor *= specularHdrScale ;
  float iridescenceIor = packedIriParams ? mix(1.0, 3.0, float(pc.bsdf3Packed & 255u) / 255.0) : 1.3 ;
  float iridescenceThicknessMin = packedIriParams ? (float((pc.bsdf3Packed >> 8u) & 255u) / 255.0) * 800.0 : 100.0 ;
  float iridescenceThicknessMax = packedIriParams ? (float((pc.bsdf3Packed >> 16u) & 255u) / 255.0) * 800.0 : 400.0 ;
  if(packedIriParams){
    attenuationColor = vec3(1.0) ;
    attenuationDist = 1.0 ;
  }

  float clearcoat = float(pc.bsdf4Packed & 255u) / 255.0 ;
  float clearcoatRoughness = max(float((pc.bsdf4Packed >> 8u) & 255u) / 255.0, 0.04) ;
  if(normalMapVariance > 0.0001){
     roughness = min(1.0, sqrt(roughness * roughness + normalMapVariance * 0.45)) ;
     clearcoatRoughness = min(1.0, sqrt(clearcoatRoughness * clearcoatRoughness + normalMapVariance * 0.50)) ;
  }
  float anisotropyStrength = float((pc.bsdf4Packed >> 16u) & 255u) / 255.0 ;
  float dispersion = (float((pc.bsdf4Packed >> 24u) & 255u) / 255.0) * 10.0 ;

	  float diffuseTransmission = float(pc.bsdf5Packed & 255u) / 255.0 ;
	  float refractionFactor = float((pc.bsdf5Packed >> 8u) & 255u) / 255.0 ;
	  float subsurfaceFactor = float((pc.bsdf5Packed >> 16u) & 255u) / 255.0 ;
	  float iridescenceThicknessNorm = 1.0 ;
	  vec3 diffuseTransmissionColor = extDiffuseTransmissionColor ? sheenColor : vec3(1.0) ;
	  if(extDiffuseTransmissionColor){ sheenColor = vec3(0.0) ; }
	  float sheenMaxMaterial = max3(sheenColor) ;

  vec3 baseColor = texBaseLinear * baseTint.rgb ;
  if(vertexColorPrimary){ baseColor = baseTint.rgb ; }
  uint occIndex = pc.occlusionTexIndex & 0xFFFFu ;
  vec2 occUV = mapUv(pc.occlusionTexIndex, pc.occlusionUvXf0, pc.occlusionUvXf1) ;
  bool hasOccTexture = (pc.occlusionTexIndex & 0x80000000u) == 0u ;
  if(hasOccTexture && occIndex < 1024u){
     vec4 occTex = texture(texSamplers[nonuniformEXT(occIndex)], occUV) ;
     if(extSpecularMap){
        specularFactor = clamp(specularFactor * occTex.a, 0.0, 1.0) ;
     } else if(extSheenColorMap){
        sheenColor *= occTex.rgb ;
     } else if(extClearcoatMap){
        clearcoat = clamp(clearcoat * occTex.r, 0.0, 1.0) ;
     } else if(extThicknessMap){
        thickness = clamp(thickness * occTex.g, 0.0, 4.0) ;
     } else if(extIridescenceMap){
        iridescence = clamp(iridescence * smoothstep(0.50, 0.85, occTex.r), 0.0, 1.0) ;
     }
  }
  vec2 anisotropyDir = vec2(1.0, 0.0) ;
  vec2 anisotropyBasisUV = baseUV ;
  uint ext2Word = pc.mrTexIndex ;
  bool extClearcoatRoughMap = false ;
  if((ext2Word & 0x80000000u) == 0u){
     uint ext2Index = ext2Word & 0xFFFFu ;
     uint ext2Type = (ext2Word >> 24u) & 0x0Fu ;
     vec2 ext2UV = applyUvXf(((ext2Word & 0x10000u) != 0u) ? vUV2 : vUV, pc.mrUvXf0, pc.mrUvXf1) ;
     float anisotropyRotation = decodeAnisoRot7((ext2Word >> 17u) & 127u) ;
     float anisoCos = cos(anisotropyRotation) ;
     float anisoSin = sin(anisotropyRotation) ;
     if(ext2Type == 4u){
        anisotropyBasisUV = ext2UV ;
        anisotropyDir = vec2(anisoCos, anisoSin) ;
     }
     if(ext2Index < 1024u){
        vec4 ext2Tex = texture(texSamplers[nonuniformEXT(ext2Index)], ext2UV) ;
        if(ext2Type == 1u){
           transmission = clamp(transmission * ext2Tex.r, 0.0, 1.0) ;
        } else if(ext2Type == 2u){
           sheenRoughness = clamp(sheenRoughness * ext2Tex.a, 0.0, 1.0) ;
        } else if(ext2Type == 3u){
           iridescenceThicknessNorm = clamp(ext2Tex.g, 0.0, 1.0) ;
        } else if(ext2Type == 4u){
           anisotropyStrength = clamp(anisotropyStrength * ext2Tex.b, 0.0, 1.0) ;
           vec2 dir = ext2Tex.rg * 2.0 - 1.0 ;
           if(dot(dir, dir) > 1e-5){
              dir = normalize(dir) ;
              anisotropyDir = vec2(dir.x * anisoCos - dir.y * anisoSin, dir.x * anisoSin + dir.y * anisoCos) ;
           }
        } else if(ext2Type == 5u){
           extClearcoatRoughMap = true ;
           clearcoatRoughness = max(clearcoatRoughness * ext2Tex.g, 0.04) ;
        } else if(ext2Type == 6u){
           diffuseTransmission = clamp(diffuseTransmission * ext2Tex.a, 0.0, 1.0) ;
           if(!extDiffuseTransmissionColor){
              diffuseTransmissionColor = ext2Tex.rgb ;
           }
        }
     }
  }
  uint emissiveIndex = pc.emissiveTexIndex ;
  bool extTransmissionMap = (emissiveIndex & 0x20000u) != 0u ;
  bool extDiffuseTransmissionMap = (emissiveIndex & 0x40000u) != 0u ;
  if((emissiveIndex & 0x80000000u) != 0u){ emissiveIndex = 0xFFFFFFFFu ; }
  else { emissiveIndex = emissiveIndex & 0xFFFFu ; }
  vec2 emissiveUV = mapUv(pc.emissiveTexIndex, pc.emissiveUvXf0, pc.emissiveUvXf1) ;
  if(extTransmissionMap && emissiveIndex < 1024u){
     transmission = clamp(transmission * texture(texSamplers[nonuniformEXT(emissiveIndex)], emissiveUV).r, 0.0, 1.0) ;
  }
  if(extDiffuseTransmissionMap && emissiveIndex < 1024u){
     diffuseTransmission = clamp(diffuseTransmission * texture(texSamplers[nonuniformEXT(emissiveIndex)], emissiveUV).a, 0.0, 1.0) ;
  }
  if(extDiffuseTransmissionColor && emissiveIndex < 1024u && !extDiffuseTransmissionMap){
     diffuseTransmissionColor *= texture(texSamplers[nonuniformEXT(emissiveIndex)], emissiveUV).rgb ;
  }
  // Viewer autofit is a presentation transform, not authored material depth.
  // Scaling thickness here makes volume/transmission assets over-attenuate.
  thickness = clamp(thickness, 0.0, 4.0) ;
  vec3 diffuseTransmissionAlbedo = baseColor * diffuseTransmissionColor ;
  bool diffuseTransmissionAlphaCard = alphaMode != 0u && diffuseTransmission > 0.001 ;
  vec3 diffuseTransmissionScatterAlbedo = diffuseTransmissionAlbedo ;
  vec3 diffuseTransmissionCardFill = diffuseTransmissionScatterAlbedo ;
  if(diffuseTransmissionAlphaCard){
     // Foliage cards carry thin BTDF energy in dark leaf textures; scattering
     // through the same albedo crushes them against the gallery background.
     diffuseTransmissionScatterAlbedo = mix(
        diffuseTransmissionAlbedo,
        sqrt(max(diffuseTransmissionAlbedo, vec3(0.0))),
        0.92
     ) ;
     diffuseTransmissionCardFill = mix(diffuseTransmissionScatterAlbedo, sqrt(max(baseColor, vec3(0.0))), 0.55) ;
  }
  if(extSpecularColorMap && emissiveIndex < 1024u && !specGlossWorkflow){
     specularColor *= texture(texSamplers[nonuniformEXT(emissiveIndex)], emissiveUV).rgb ;
  }
  if(specGlossWorkflow){
     const float dielectricSpecular = 0.04 ;
     vec3 sgDiffuse = clamp(baseColor, vec3(0.0), vec3(1.0)) ;
     vec3 sgSpecular = clamp(specularColor, vec3(0.0), vec3(1.0)) ;
     float specStrength = max3(sgSpecular) ;
     float oneMinusSpecularStrength = clamp(1.0 - specStrength, 0.0, 1.0) ;
     float sgMetallic = solveSpecGlossMetallic(sgDiffuse, sgSpecular, oneMinusSpecularStrength) ;
     float specChroma = max(max(abs(sgSpecular.r - sgSpecular.g), abs(sgSpecular.g - sgSpecular.b)), abs(sgSpecular.b - sgSpecular.r)) ;
     float diffuseChroma = max(max(abs(sgDiffuse.r - sgDiffuse.g), abs(sgDiffuse.g - sgDiffuse.b)), abs(sgDiffuse.b - sgDiffuse.r)) ;
     float specDiffuseAlign = dot(normalize(sgSpecular + vec3(0.001)), normalize(sgDiffuse + vec3(0.001))) ;
     float coloredMetalHint = clamp((specChroma * 2.70 + diffuseChroma * 0.45 + (specDiffuseAlign - 0.72) * 1.65) * smoothstep(0.16, 0.54, specStrength), 0.0, 1.0) ;
     float strongColoredSpec = smoothstep(0.62, 0.92, specStrength) * smoothstep(0.08, 0.24, specChroma) ;
     sgMetallic = clamp(max(sgMetallic, max(coloredMetalHint, strongColoredSpec)), 0.0, 1.0) ;
     vec3 baseFromDiffuse = sgDiffuse * oneMinusSpecularStrength / (1.0 - dielectricSpecular) / max(1.0 - sgMetallic, 1e-4) ;
     vec3 baseFromSpecular = (sgSpecular - vec3(dielectricSpecular) * (1.0 - sgMetallic)) / max(sgMetallic, 1e-4) ;
     baseColor = clamp(mix(baseFromDiffuse, baseFromSpecular, smoothstep(0.0, 1.0, sgMetallic)), vec3(0.0), vec3(1.0)) ;
     metallic = sgMetallic ;
     specularColor = vec3(1.0) ;
     specGlossWorkflow = false ;
  }
  vec3 outLin ;
  bool finalEmissiveOnlySurface = false ;
  {
      vec3 diffuseColor = baseColor * (1.0 - metallic) ;
      bool emissiveOnlySurface = dot(baseColor, baseColor) <= 0.0006 && metallic <= 0.01 && emissiveIndex < 1024u && !extSpecularColorMap ;
      finalEmissiveOnlySurface = emissiveOnlySurface ;
      bool specularOnlySurface = dot(baseColor, baseColor) <= 0.0006 && metallic <= 0.01 && (specularFactor > 0.01 || extSpecularMap || extSpecularColorMap) ;
      bool darkReflectiveSurface = dot(baseColor, baseColor) <= 0.0006 && !emissiveOnlySurface ;
      bool extensionSurface = clearcoat > 0.0 || anisotropyStrength > 0.0 || iridescence > 0.0 || transmission > 0.0 || diffuseTransmission > 0.0 || dot(sheenColor, sheenColor) > 0.0005 ;
      float safeIor = max(ior, 1.001) ;
      float dielectricF0Scalar = pow((safeIor - 1.0) / (safeIor + 1.0), 2.0) ;
      bool glossyMaterial = sourceSpecGlossWorkflow || specGlossWorkflow || extSpecularMap || extSpecularColorMap || metallic > 0.05 || clearcoat > 0.0 || transmission > 0.0 || iridescence > 0.0 ;
      float defaultSpecScale = glossyMaterial ? 0.82 : 0.06 ;
      float defaultDiffuseScale = glossyMaterial ? 0.05 : 0.24 ;
      float directSpecScale = glossyMaterial ? max(defaultSpecScale, mix(1.10, 0.34, roughness)) : mix(1.10, 0.24, roughness) ;
      vec3 dielectricF0 = min(vec3(max(dielectricF0Scalar, 0.04)) * max(specularColor, vec3(0.0)), vec3(1.0)) * clamp(specularFactor, 0.0, 1.0) ;
      float dielectricF90 = clamp(specularFactor, 0.0, 1.0) ;
      vec3 F0 = specGlossWorkflow ? clamp(specularColor, vec3(0.0), vec3(1.0)) : mix(dielectricF0, baseColor, metallic) ;
      vec3 envF90 = specGlossWorkflow ? vec3(1.0) : mix(vec3(dielectricF90), vec3(1.0), metallic) ;
      vec3 R = reflect(-V, N) ;
      vec3 envDiffuse = sampleEnvDiffuse(N) ;
      vec3 envDiffuseBack = sampleEnvDiffuse(-N) ;
      vec3 envSpec = sampleEnvSpec(normalize(mix(R, N, roughness * roughness * 0.35)), roughness) ;
      float opticalVolumeSurface = smoothstep(0.0001, 0.012, thickness) ;
      float opticalIriSurface = clamp(iridescence * max(transmission, diffuseTransmission) * opticalVolumeSurface, 0.0, 1.0) ;
      if(opticalIriSurface > 0.001){
         envSpec = mix(envSpec, opticalLightRigEnv(R, roughness), clamp(opticalIriSurface * (0.74 + 0.12 * (1.0 - roughness)), 0.0, 0.84)) ;
      }
      float NdotV = max(dot(N, V), 0.001) ;
      vec3 fresnelV = specGlossWorkflow ? F_Schlick(F0, NdotV) : (dielectricF0 + (vec3(dielectricF90) - dielectricF0) * pow(1.0 - NdotV, 5.0)) ;
      if(!specGlossWorkflow){
        fresnelV = mix(fresnelV, F0 + (1.0 - F0) * pow(1.0 - NdotV, 5.0), metallic) ;
      }
      float fresnelMaxV = max(max(fresnelV.r, fresnelV.g), fresnelV.b) ;
      float diffuseRemain = mix(1.0, 0.82, diffuseTransmission) ;
      float roughDiffuse = glossyMaterial ? mix(0.16, 1.0, roughness) : 1.0 ;
      bool diffuseBoostExtension = transmission <= 0.001 && diffuseTransmission <= 0.001 && (clearcoat > 0.0 || iridescence > 0.0 || extSpecularMap || extSpecularColorMap) ;
      float extensionDiffuseBoost = diffuseBoostExtension ? 1.18 : 1.0 ;
      vec3 diffuseEnergy = diffuseColor * (1.0 - fresnelMaxV) * extensionDiffuseBoost ;
      vec3 Lo = vec3(0.0) ;
      float envAniso = 1.0 ;
      vec3 dPdx = dFdx(vWorldPos) ;
      vec3 dPdy = dFdy(vWorldPos) ;
      vec2 anisoUV = anisotropyStrength > 0.001 ? anisotropyBasisUV : baseUV ;
      vec2 dUVdx = dFdx(anisoUV) ;
      vec2 dUVdy = dFdy(anisoUV) ;
      vec3 tangent = dPdx * dUVdy.y - dPdy * dUVdx.y ;
      if(length(tangent) < 1e-5){ tangent = normalize(abs(N.z) < 0.999 ? cross(vec3(0.0, 0.0, 1.0), N) : cross(vec3(0.0, 1.0, 0.0), N)) ; }
      else { tangent = normalize(tangent - N * dot(N, tangent)) ; }
      vec3 bitangent = normalize(cross(N, tangent)) ;
      vec3 anisoTangent = tangent ;
      vec3 anisoBitangent = bitangent ;
      if(anisotropyStrength > 0.001 && dot(anisotropyDir, anisotropyDir) > 1e-5){
         vec2 dir = normalize(anisotropyDir) ;
         anisoTangent = normalize(tangent * dir.x + bitangent * dir.y) ;
         anisoBitangent = normalize(cross(N, anisoTangent)) ;
      }
      envAniso = mix(1.0, clamp(abs(dot(R, anisoTangent)) * 1.9 + abs(dot(R, anisoBitangent)) * 0.30, 0.30, 2.10), anisotropyStrength) ;
      float anisoEnvLobe = pow(clamp(abs(dot(normalize(R + anisoTangent * 0.6), anisoTangent)), 0.0, 1.0), mix(3.0, 28.0, 1.0 - roughness)) ;
      float darkSurfaceScale = 1.0 ;
      float emissiveOnlySpecScale = 1.0 ;
      float specularOnlyBoost = specularOnlySurface ? 1.15 : 1.0 ;
      if(darkReflectiveSurface && metallic <= 0.05 && transmission <= 0.001 && diffuseTransmission <= 0.001 && clearcoat <= 0.001 && iridescence <= 0.001){
         darkSurfaceScale = mix(0.18, 0.42, clamp(specularFactor, 0.0, 1.0)) ;
      }
      float darkReflectiveBoost = darkReflectiveSurface ? mix(0.35, 4.40, metallic) : 1.0 ;
      float darkClearcoatEnvBoost = 1.0 ;
      if(clearcoat > 0.001){
         float baseEnergyNorm = clamp(dot(baseColor, baseColor) / 0.05, 0.0, 1.0) ;
         float mappedClearcoatBoost = (extClearcoatMap || extClearcoatRoughMap) ? 1.55 : 1.12 ;
         darkClearcoatEnvBoost = mix(2.70, 1.0, baseEnergyNorm) * mappedClearcoatBoost ;
      }
      float anySceneLight = 0.0 ;
      for(int li = 0 ; li < 8; li++){
         vec4 lPosType  = scene.lightPosType[li] ;
         vec4 lColRange = scene.lightColorRange[li] ;
         vec4 lDirOuter = scene.lightDirOuter[li] ;
         vec3 lColor = lColRange.rgb ;
         if(dot(lColor, lColor) < 0.0001){ continue ; }
         anySceneLight = 1.0 ;
         float lType = lPosType.w ;
         float lRange = lColRange.w ;
         vec3 L ;
         float attenuation = 1.0 ;
         if(lType < 0.5){
            L = normalize(-lDirOuter.xyz) ;
         } else {
            vec3 toLight = lPosType.xyz - vWorldPos ;
            float dist2 = max(dot(toLight, toLight), 0.0001) ;
            float invDist = inversesqrt(dist2) ;
            float dist = dist2 * invDist ;
            L = toLight * invDist ;
            attenuation = 1.0 / max(dist2, 0.04) ;
            if(lRange > 0.0){
               float invRange = 1.0 / lRange ;
               float nd = clamp(dist * invRange, 0.0, 1.0) ;
               float fall = clamp(1.0 - nd * nd * nd * nd, 0.0, 1.0) ;
               // glTF range is a cutoff distance, not an energy multiplier.
               // Multiplying by range^2 blows out punctual-light proofs badly.
               attenuation *= fall * fall ;
            }
            if(lType > 1.5){
               float outerCos = lDirOuter.w ;
               float innerCos = outerCos + 0.1 ;
               float spotAngle = dot(-L, normalize(lDirOuter.xyz)) ;
               attenuation *= clamp((spotAngle - outerCos) / max(innerCos - outerCos, 0.001), 0.0, 1.0) ;
            }
         }
         float NdotLFront = sat1(dot(N, L)) ;
         float NdotLBack = sat1(dot(-N, L)) ;
         float NdotL = max(NdotLFront, NdotLBack * 0.85) ;
         if(NdotL <= 0.0){ continue ; }
         vec3 H = normalize(L + V) ;
         float NoH = sat1(dot(N, H)) ;
         float VoH = sat1(dot(V, H)) ;
         vec3 F = specGlossWorkflow ? F_Schlick(F0, VoH) : materialFresnel(dielectricF0, dielectricF90, F0, metallic, VoH) ;
         float D = anisotropyStrength > 0.001 ? D_GGX_Aniso(H, N, anisoTangent, anisoBitangent, roughness, anisotropyStrength) : D_GGX(NoH, roughness) ;
         float Vis = anisotropyStrength > 0.001 ? V_GGX_Aniso(V, L, N, anisoTangent, anisoBitangent, roughness, anisotropyStrength) : V_Smith(max(NdotV, 0.001), max(NdotL, 0.001), roughness) ;
         vec3 spec = D * Vis * F ;
         float baseEnergy = 1.0 - max3(F) ;
         vec3 diff = baseEnergy * (1.0 - transmission) * diffuseRemain * diffuseColor * roughDiffuse * extensionDiffuseBoost / PI ;
         vec3 diffT = diffuseTransmission > 0.001 ? baseEnergy * (1.0 - transmission) * diffuseTransmission * diffuseTransmissionScatterAlbedo / PI : vec3(0.0) ;
         vec3 sheen = vec3(0.0) ;
         float sheenMax = max3(sheenColor) ;
         if(sheenMax > 0.001){
            float sheenNoH = sat1(dot(N, H)) ;
            sheen = sheenColor * D_Charlie(sheenNoH, max(sheenRoughness, 0.05)) * V_Sheen(max(NdotV, 0.001), max(NdotL, 0.001)) ;
         }
         float sheenScale = 1.0 - sheenMax * 0.35 ;
         vec3 radiance = lColor * attenuation ;
         float coatLayer = 0.0 ;
         vec3 coat = vec3(0.0) ;
         if(clearcoat > 0.0){
            vec3 coatN = coatNormal ;
            vec3 coatH = normalize(V + L) ;
            float coatNoL = sat1(dot(coatN, L)) ;
            float coatNoH = sat1(dot(coatN, coatH)) ;
            float coatVoH = sat1(dot(V, coatH)) ;
            vec3 coatF = F_Schlick(vec3(0.04), coatVoH) ;
            coat = clearcoat * D_GGX(coatNoH, clearcoatRoughness) * V_Smith(max(sat1(dot(coatN, V)), 0.001), max(coatNoL, 0.001), clearcoatRoughness) * coatF ;
            coatLayer = clearcoat * (0.03 + 0.30 * pow5(1.0 - max(sat1(dot(coatN, V)), 0.001))) ;
         }
         float baseLayer = 1.0 - coatLayer ;
         Lo += ((((diff + spec) * sheenScale + sheen) * baseLayer + coat) * radiance * NdotL + diffT * baseLayer * radiance * sat1(dot(-N, L)) * 0.55) * darkSurfaceScale * emissiveOnlySpecScale * specularOnlyBoost ;
      }
      float texturedSheenDamp = baseIndex < 1024u ? 0.42 : 1.0 ;
      if(anySceneLight < 0.5){
         vec3 Ls[3] = vec3[3](
           normalize(vec3(0.25, 0.90, 0.32)),
           normalize(vec3(-0.75, 0.45, -0.18)),
           normalize(vec3(0.12, 0.25, -0.95))
         ) ;
         vec3 Cs[3] = vec3[3](
           vec3(0.92, 0.90, 0.86),
           vec3(0.32, 0.32, 0.34),
           vec3(0.12, 0.12, 0.13)
         ) ;
         Lo = vec3(0.0) ;
         for(int i = 0 ; i < 3; i++){
            vec3 L = Ls[i] ;
            vec3 H = normalize(V + L) ;
            float NoLFront = sat1(dot(N, L)) ;
            float NoLBack = sat1(dot(-N, L)) ;
            float twoSidedDiffuse = (alphaMode != 0u || diffuseTransmission > 0.001) ? 0.82 : 0.30 ;
            float NoLDiff = max(NoLFront, NoLBack * twoSidedDiffuse) ;
            float NoLSpec = NoLFront ;
            float NoH = sat1(dot(N, H)) ;
            float VoH = sat1(dot(V, H)) ;
            vec3 F = F_Schlick(F0, VoH) ;
            float D = anisotropyStrength > 0.001 ? D_GGX_Aniso(H, N, anisoTangent, anisoBitangent, roughness, anisotropyStrength) : D_GGX(NoH, roughness) ;
            float Vis = anisotropyStrength > 0.001 ? V_GGX_Aniso(V, L, N, anisoTangent, anisoBitangent, roughness, anisotropyStrength) : V_Smith(max(NdotV, 0.001), max(NoLSpec, 0.001), roughness) ;
            vec3 spec = D * Vis * F ;
            vec3 coat = vec3(0.0) ;
            float coatLayer = 0.0 ;
            if(clearcoat > 0.0){
               float coatNdotV = max(sat1(dot(coatNormal, V)), 0.001) ;
               vec3 coatH = normalize(V + L) ;
               float coatNoL = sat1(dot(coatNormal, L)) ;
               float coatNoH = sat1(dot(coatNormal, coatH)) ;
               float coatVoH = sat1(dot(V, coatH)) ;
               vec3 coatF = F_Schlick(vec3(0.04), coatVoH) ;
               coat = clearcoat * D_GGX(coatNoH, clearcoatRoughness) * V_Smith(coatNdotV, max(coatNoL, 0.001), clearcoatRoughness) * coatF ;
               coatLayer = clearcoat * (0.03 + 0.30 * pow5(1.0 - coatNdotV)) ;
            }
            float baseEnergy = 1.0 - max3(F) ;
            vec3 diff = baseEnergy * (1.0 - transmission) * diffuseRemain * diffuseColor * roughDiffuse * extensionDiffuseBoost / PI ;
            vec3 diffT = diffuseTransmission > 0.001 ? baseEnergy * (1.0 - transmission) * diffuseTransmission * diffuseTransmissionScatterAlbedo / PI : vec3(0.0) ;
            vec3 sheen = vec3(0.0) ;
            float sheenMax = max3(sheenColor) ;
            if(sheenMax > 0.001){
               float sheenNoH = sat1(dot(N, H)) ;
               sheen = sheenColor * D_Charlie(sheenNoH, max(sheenRoughness, 0.05)) * V_Sheen(max(NdotV, 0.001), max(NoLSpec, 0.001)) ;
            }
            float sheenScale = 1.0 - sheenMax * 0.35 ;
            float baseLayer = 1.0 - coatLayer ;
            Lo += (diff * sheenScale * baseLayer * NoLDiff + (spec * sheenScale + sheen) * baseLayer * NoLSpec + coat * NoLSpec) * Cs[i] ;
            Lo += diffT * baseLayer * Cs[i] * NoLBack * 1.35 ;
         }
         vec3 envF = envBrdfApprox(F0, envF90, roughness, max(NdotV, 0.0)) ;
         float coatNdotV = max(sat1(dot(coatNormal, V)), 0.001) ;
         float clearcoatFv = clearcoat > 0.001 ? clearcoat * (0.03 + 0.30 * pow5(1.0 - coatNdotV)) : 0.0 ;
         float clearcoatBaseScaleV = 1.0 - clearcoatFv ;
         float sheenScaleV = 1.0 - max3(sheenColor) * 0.35 ;
         float baseEnergyV = 1.0 - max3(fresnelV) ;
         vec3 envDiffuseTerm = envDiffuse * diffuseColor * (1.0 - transmission) * diffuseRemain * baseEnergyV * sheenScaleV * roughDiffuse * extensionDiffuseBoost ;
         vec3 envSpecTerm = envSpec * envF ;
         if(diffuseTransmission > 0.001){
            envDiffuseTerm += envDiffuseBack * diffuseTransmissionScatterAlbedo * (1.0 - transmission) * diffuseTransmission * baseEnergyV * 1.10 ;
         }
         float iblDiffuseScale = glossyMaterial ? mix(0.18, 0.62, roughness) : 0.90 ;
         float iblSpecScale = glossyMaterial ? mix(5.10, 1.45, roughness) : mix(2.80, 1.08, roughness) ;
         float metalness = clamp(metallic, 0.0, 1.0) ;
         iblDiffuseScale *= mix(1.0, 0.10, metalness) ;
         iblSpecScale = mix(iblSpecScale, mix(1.85, 0.92, roughness), metalness) ;
         iblSpecScale *= sourceSpecGlossWorkflow ? mix(1.24, 1.08, roughness) : 1.0 ;
         iblSpecScale *= mix(1.0, 2.35, iridescence * (1.0 - max(transmission, diffuseTransmission) * 0.55) * (1.0 - metalness * 0.72)) ;
         vec3 envTerm = envDiffuseTerm * iblDiffuseScale + envSpecTerm * iblSpecScale ;
         float darkSheenCloth = smoothstep(0.02, 0.30, sheenMaxMaterial) * (1.0 - smoothstep(0.035, 0.18, max3(baseColor))) * (1.0 - metallic) * (1.0 - max(transmission, diffuseTransmission)) * texturedSheenDamp ;
         if(darkSheenCloth > 0.001){
            vec3 velvetTint = mix(vec3(max3(sheenColor) * 0.55), sqrt(max(sheenColor, vec3(0.0))), 0.80) ;
            vec3 velvetFill = envDiffuse * velvetTint * (0.10 + 0.30 * sheenRoughness) ;
            envTerm += velvetFill * darkSheenCloth ;
         }
         Lo += envTerm * clearcoatBaseScaleV ;
         if(clearcoat > 0.001){
            vec3 Rc = reflect(-V, coatNormal) ;
            Lo += clearcoat * sampleEnvSpec(Rc, clearcoatRoughness) * F_Rough(vec3(0.04), coatNdotV, clearcoatRoughness) * (0.72 * darkClearcoatEnvBoost) ;
         }
      } else {
         vec3 envF = envBrdfApprox(F0, envF90, roughness, max(NdotV, 0.0)) ;
         float coatNdotV = max(sat1(dot(coatNormal, V)), 0.001) ;
         float clearcoatFv = clearcoat > 0.001 ? clearcoat * (0.03 + 0.30 * pow5(1.0 - coatNdotV)) : 0.0 ;
         float clearcoatBaseScaleV = 1.0 - clearcoatFv ;
         float sheenScaleV = 1.0 - max3(sheenColor) * 0.35 ;
         float baseEnergyV = 1.0 - max3(fresnelV) ;
         vec3 envDiffuseTerm = envDiffuse * diffuseColor * (1.0 - transmission) * diffuseRemain * baseEnergyV * sheenScaleV * roughDiffuse * extensionDiffuseBoost ;
         vec3 envSpecTerm = envSpec * envF ;
         if(diffuseTransmission > 0.001){
            envDiffuseTerm += envDiffuseBack * diffuseTransmissionScatterAlbedo * (1.0 - transmission) * diffuseTransmission * baseEnergyV * 1.10 ;
         }
         float sceneIblDiffuseScale = glossyMaterial ? mix(0.14, 0.40, roughness) : 0.55 ;
         float sceneIblSpecScale = glossyMaterial ? mix(1.28, 0.62, roughness) : mix(0.58, 0.24, roughness) ;
         float sceneMetalness = clamp(metallic, 0.0, 1.0) ;
         sceneIblDiffuseScale *= mix(1.0, 0.16, sceneMetalness) ;
         sceneIblSpecScale = mix(sceneIblSpecScale, mix(1.10, 0.52, roughness), sceneMetalness) ;
         sceneIblSpecScale *= sourceSpecGlossWorkflow ? mix(1.22, 1.06, roughness) : 1.0 ;
         sceneIblSpecScale *= mix(1.0, 1.55, iridescence * (1.0 - max(transmission, diffuseTransmission) * 0.45) * (1.0 - sceneMetalness * 0.65)) ;
         vec3 envTerm = envDiffuseTerm * sceneIblDiffuseScale + envSpecTerm * sceneIblSpecScale ;
         Lo += envTerm * clearcoatBaseScaleV * darkReflectiveBoost * darkSurfaceScale * emissiveOnlySpecScale * specularOnlyBoost ;
         if(clearcoat > 0.001){
            vec3 Rc = reflect(-V, coatNormal) ;
            Lo += clearcoat * sampleEnvSpec(Rc, clearcoatRoughness) * F_Rough(vec3(0.04), coatNdotV, clearcoatRoughness) * (0.30 * darkClearcoatEnvBoost) ;
         }
      }
      if(anySceneLight < 0.5){
         vec3 Lsun = normalize(vec3(-0.55, 0.82, 0.18)) ;
         vec3 Hsun = normalize(Lsun + V) ;
         float specPow = mix(256.0, 8.0, roughness) ;
         float NdotLsunFront = sat1(dot(N, Lsun)) ;
         float NdotLsunBack = sat1(dot(-N, Lsun)) ;
         float NdotLsunDiffuse = max(NdotLsunFront, NdotLsunBack * ((alphaMode != 0u || diffuseTransmission > 0.001) ? 0.65 : 0.22)) ;
         float sunAniso = mix(1.0, clamp(abs(dot(Hsun, anisoTangent)) * 1.8 + abs(dot(Hsun, anisoBitangent)) * 0.35, 0.35, 2.0), anisotropyStrength) ;
         vec3 specsun = pow(max(dot(N, Hsun), 0.0), specPow * sunAniso) * F0 ;
         float skyKey = glossyMaterial ? (extensionSurface ? 0.18 : 0.12) : 0.04 ;
         float skySpec = glossyMaterial ? ((extensionSurface ? 0.72 : 0.44) * darkReflectiveBoost) : 0.12 ;
         Lo += diffuseEnergy * NdotLsunDiffuse * skyKey * darkSurfaceScale ;
         Lo += specsun * NdotLsunFront * mix(0.28, 0.52, sqrt(specularFactor)) * (1.0 - roughness * 0.45) * directSpecScale * skySpec * mix(1.0, 0.34, metallic) * darkSurfaceScale * emissiveOnlySpecScale * specularOnlyBoost ;
      }
      float sheenTerm = pow(1.0 - NdotV, mix(8.0, 2.0, sheenRoughness)) ;
      Lo += sheenColor * sheenTerm * (1.0 - metallic) * mix(0.035, 0.10, 1.0 - sheenRoughness) ;
      Lo += envSpec * sheenColor * sheenTerm * (1.0 - metallic) * mix(0.02, 0.07, 1.0 - sheenRoughness) ;
      float darkSheenClothLate = smoothstep(0.02, 0.30, sheenMaxMaterial) * (1.0 - smoothstep(0.035, 0.18, max3(baseColor))) * (1.0 - metallic) * (1.0 - max(transmission, diffuseTransmission)) * texturedSheenDamp ;
      if(darkSheenClothLate > 0.001){
         vec3 darkSheenTint = mix(vec3(max3(sheenColor) * 0.55), sqrt(max(sheenColor, vec3(0.0))), 0.80) ;
         float velvetView = 0.48 + 0.52 * pow(1.0 - NdotV, mix(1.4, 0.45, sheenRoughness)) ;
         Lo += darkSheenTint * darkSheenClothLate * (0.025 + 0.055 * velvetView + 0.020 * sheenTerm) ;
         Lo = max(Lo, darkSheenTint * darkSheenClothLate * 0.018) ;
      }
	      if(metallic > 0.85 && roughness <= 0.08 && transmission <= 0.001 && diffuseTransmission <= 0.001 && clearcoat <= 0.001 && iridescence <= 0.001 && baseIndex >= 1024u && !hasMrTexture){
	         vec3 metalLightRigSpec = opticalLightRigEnv(R, roughness) * mix(vec3(0.72), max(baseColor, vec3(0.42)), 0.58) ;
	         Lo = max(Lo, metalLightRigSpec * mix(0.62, 0.92, 1.0 - roughness)) ;
	      }
	      if(metallic > 0.85 && roughness <= 0.62 && transmission <= 0.001 && diffuseTransmission <= 0.001 && clearcoat <= 0.001 && iridescence <= 0.001 && baseIndex >= 1024u && !hasMrTexture){
	         vec3 roughMetalEnv = mix(sampleEnvSpec(R, roughness), opticalLightRigEnv(R, roughness), 0.28) ;
	         vec3 roughMetalTint = mix(vec3(0.30), max(baseColor, vec3(0.22)), 0.72) ;
	         float roughMetalFloor = mix(0.30, 0.11, clamp(roughness, 0.0, 1.0)) ;
	         Lo = max(Lo, roughMetalEnv * roughMetalTint * roughMetalFloor) ;
	      }
	      if(hasMrTexture && metallic > 0.30 && roughness <= 0.78 && transmission <= 0.001 && diffuseTransmission <= 0.001 && clearcoat <= 0.001 && iridescence <= 0.001 && baseIndex < 1024u){
	         vec3 texturedMetalEnv = max(mix(sampleEnvSpec(R, roughness), opticalLightRigEnv(R, roughness), 0.72), fallbackEnvSpec(R, roughness) * 0.58) ;
	         vec3 texturedMetalTint = mix(vec3(0.18), max(baseColor, vec3(0.11)), 0.92) ;
	         if(sourceSpecGlossWorkflow){
	            texturedMetalTint = mix(texturedMetalTint, pow(max(baseColor, vec3(0.018)), vec3(0.82)), 0.42) ;
	         }
	         float texturedMetalGate = smoothstep(0.30, 0.82, metallic) * (1.0 - smoothstep(0.72, 1.0, roughness)) ;
	         vec3 texturedMetalTarget = texturedMetalEnv * texturedMetalTint * mix(sourceSpecGlossWorkflow ? 2.15 : 1.85, sourceSpecGlossWorkflow ? 0.82 : 0.72, clamp(roughness, 0.0, 1.0)) ;
	         Lo = mix(Lo, max(Lo, texturedMetalTarget), texturedMetalGate) ;
	      }
	      if(diffuseTransmissionAlphaCard){
         float foliageWrap = 0.38 + 0.62 * max(dot(-N, V), 0.0) ;
         Lo += diffuseTransmissionCardFill * foliageWrap * (1.05 + 1.30 * diffuseTransmission) ;
      }
      if(iridescence > 0.0){
         float iriThicknessNm = mix(iridescenceThicknessMin, iridescenceThicknessMax, clamp(iridescenceThicknessNorm, 0.0, 1.0)) ;
         float etaFilm = max(iridescenceIor, 1.001) ;
         float etaBase = max(ior, 1.001) ;
         float cosTheta1 = clamp(NdotV, 0.0, 1.0) ;
         float sinTheta2Sq = max(1.0 - cosTheta1 * cosTheta1, 0.0) / max(etaFilm * etaFilm, 1.000001) ;
         float cosTheta2 = sqrt(max(1.0 - sinTheta2Sq, 0.0)) ;
         vec3 wavelengths = vec3(650.0, 510.0, 475.0) ;
         vec3 phase = (4.0 * PI * etaFilm * iriThicknessNm * cosTheta2) / wavelengths ;
         vec3 fringe = 0.5 - 0.5 * cos(phase) ;
         float filmF0 = pow((etaFilm - 1.0) / (etaFilm + 1.0), 2.0) ;
         float baseFilmF0 = pow((etaBase - etaFilm) / max(etaBase + etaFilm, 1.000001), 2.0) ;
         float iriContrast = clamp(abs(baseFilmF0 - filmF0) * 18.0 + 0.26, 0.0, 1.0) ;
         vec3 iriTint = mix(vec3(1.0), pow(fringe, vec3(0.85)), iriContrast) ;
         float iriRim = pow(1.0 - cosTheta1, 2.15) ;
         float iriSurfaceBoost = mix(1.0, 1.75, clamp(max(transmission, diffuseTransmission), 0.0, 1.0)) ;
         vec3 iriSpec = iriTint * (0.10 + 0.46 * iriRim) * iridescence * iriSurfaceBoost ;
         float opaqueIri = iridescence * (1.0 - max(transmission, diffuseTransmission)) ;
         float iriMetal = opaqueIri * metallic ;
         vec3 pearlIri = mix(vec3(0.94, 0.97, 0.92), iriTint, 0.24) ;
         vec3 metalIri = mix(vec3(0.02, 0.62, 0.42), iriTint, 0.16) ;
         vec3 metalRimIri = mix(vec3(0.92, 0.16, 0.66), iriTint, 0.24) ;
         Lo *= 1.0 - clamp(iridescence * 0.04, 0.0, 0.08) ;
         Lo *= 1.0 - clamp(iriMetal * 0.78, 0.0, 0.80) ;
         Lo += iriSpec * mix(0.12, 0.26, 1.0 - roughness) * mix(0.60, 0.98, sqrt(max(specularFactor, 0.0))) ;
         Lo += envSpec * iriTint * iridescence * iriSurfaceBoost * (0.12 + 0.24 * iriRim) * mix(1.0, 0.55, metallic) ;
         Lo += envDiffuse * mix(diffuseColor, vec3(1.0), 0.18) * iridescence * (1.0 - metallic) * (1.0 - max(transmission, diffuseTransmission)) * 0.16 ;
		         Lo += pearlIri * opaqueIri * (1.0 - metallic) * (0.16 + 0.18 * iriRim) ;
		         Lo += metalIri * iriMetal * (0.10 + 0.10 * iriRim) ;
		         Lo += metalRimIri * iriMetal * iriRim * 0.32 ;
		         vec3 iriLightRig = opticalLightRigEnv(R, roughness) * mix(iriTint, pow(max(iriTint, vec3(0.0)), vec3(0.70)) * vec3(0.88, 1.08, 1.22), 0.46) ;
		         Lo += iriLightRig * iridescence * (0.055 + 0.34 * iriRim + 0.10 * opaqueIri) * mix(1.0, 0.58, metallic) ;
		      }
		      if(transmission <= 0.001 && diffuseTransmission <= 0.001 && metallic <= 0.05 && thickness > 0.001 && max3(baseColor) <= 0.020){
		         Lo *= mix(0.035, 0.090, clamp(specularFactor, 0.0, 1.0)) ;
		      }
	      if(darkReflectiveSurface && transmission <= 0.001 && diffuseTransmission <= 0.001 && clearcoat <= 0.001 && iridescence <= 0.001 && metallic <= 0.05){
	         float darkBaseClamp = mix(0.055, 0.125, clamp(specularFactor, 0.0, 1.0)) ;
	         vec3 darkClamped = min(Lo, vec3(darkBaseClamp)) * mix(0.55, 0.90, roughness) ;
	         float darkHighlightGate = smoothstep(0.08, 0.90, specularFactor) * (1.0 - smoothstep(0.62, 0.98, roughness)) ;
	         vec3 darkHighlight = Lo * mix(1.0, 1.35, darkHighlightGate) ;
	         Lo = mix(darkClamped, max(darkClamped, darkHighlight), clamp(darkHighlightGate, 0.0, 0.78)) ;
	      }
		      if(!extSpecularMap && !extSheenColorMap && !extClearcoatMap && !extThicknessMap && !extIridescenceMap && hasOccTexture && occIndex < 1024u){
         float occ = texture(texSamplers[nonuniformEXT(occIndex)], occUV).r ;
         float occMix = occStrength ;
         if(alphaMode != 0u || transmission > 0.001){
            // Fur/foliage coverage cards and translucent BTDF/volume materials
            // already lose visual density through coverage/scatter. Applying
            // baked AO on top crushes proof scenes far below the references.
            occMix = 0.0 ;
         } else if(diffuseTransmission > 0.001){
            occMix *= 0.65 ;
         } else if(sheenMaxMaterial > 0.001){
            // KHR_materials_sheen often uses near-black baseColor plus a bright
            // velvet/satin lobe. Full AO on the base layer erases that lobe.
            occMix *= mix(0.70, 0.35, smoothstep(0.03, 0.35, sheenMaxMaterial)) ;
         }
         Lo *= mix(1.0, occ, occMix) ;
      }
      if(transmission > 0.0 || diffuseTransmission > 0.0){
         vec3 surfaceLo = Lo ;
         float transDist = (bsdf3A == 255u) ? 1e9 : max(attenuationDist * attenuationDist * 10.0, 0.001) ;
         vec3 safeAtt = max(attenuationColor, vec3(0.001)) ;
         vec3 sigmaA = -log(safeAtt) / transDist ;
         float iorNorm = clamp((ior - 1.0) / 1.42, 0.0, 1.0) ;
         float roughScatter = clamp(roughness * (0.55 + 0.45 * iorNorm) + roughness * roughness * 0.55, 0.0, 1.0) ;
         float volumeScatter = clamp(thickness * (0.80 + 1.00 * iorNorm), 0.0, 1.0) ;
         float opticalVolumeGate = smoothstep(0.0001, 0.012, thickness) ;
         float viewWrap = 0.5 + 0.5 * max(dot(-N, V), 0.0) ;
         float dtMix = clamp(diffuseTransmission, 0.0, 1.0) ;
         vec3 boundaryTint = baseColor ;
         float boundaryLum = dot(boundaryTint, vec3(0.299, 0.587, 0.114)) ;
         float boundaryChroma = max3(abs(boundaryTint - vec3(boundaryLum))) ;
         float authoredThinGate = 1.0 - smoothstep(0.015, 0.20, thickness) ;
         float thinTintGate = clamp(transmission * max(1.0 - opticalVolumeGate, authoredThinGate) * (1.0 - metallic), 0.0, 1.0) ;
         float darkThinTint = thinTintGate * (1.0 - smoothstep(0.04, 0.28, boundaryLum)) ;
         float chromaThinTint = thinTintGate * clamp(boundaryChroma * 4.60, 0.0, 1.0) ;
         vec3 thinTransmissionTint = mix(vec3(1.0), max(boundaryTint, vec3(0.006)), clamp(max(darkThinTint * 0.94, chromaThinTint * 0.95), 0.0, 0.96)) ;
         float attenuationLum = dot(attenuationColor, vec3(0.299, 0.587, 0.114)) ;
         vec3 attenuationBias = vec3(1.0) - attenuationColor ;
         float attenuationChroma = max3(abs(attenuationColor - vec3(attenuationLum))) ;
         float attenuationTintStrength = clamp(max3(attenuationBias) * 1.40 + attenuationChroma * 1.80, 0.0, 1.0) ;
	         float attenuationPresence = clamp(attenuationTintStrength * (1.0 + volumeScatter * 0.35), 0.0, 1.0) ;
		         float tintChromaWeight = clamp(attenuationChroma * 3.8, 0.0, 1.0) ;
		         tintChromaWeight *= tintChromaWeight ;
		         float neutralDispersionGlass = clamp(dispersion * 0.50, 0.0, 1.0) * (1.0 - tintChromaWeight) ;
		         float amberVolumeCue = clamp(tintChromaWeight * attenuationPresence * smoothstep(0.52, 0.92, attenuationColor.r) * (1.0 - smoothstep(0.14, 0.36, attenuationColor.b)), 0.0, 1.0) ;
			         float texturedBoundaryGate = baseIndex < 1024u ? 1.0 : 0.0 ;
		         float boundaryVolumeStrength = boundaryChroma * 5.20 + texturedBoundaryGate * max(boundaryLum - 0.08, 0.0) * 0.90 ;
			         float boundaryTextureCarrier = 1.0 - attenuationPresence * (1.0 - texturedBoundaryGate * 0.34) ;
			         float boundaryVolumeTint = clamp(transmission * opticalVolumeGate * boundaryTextureCarrier * boundaryVolumeStrength * smoothstep(0.06, 0.66, boundaryLum), 0.0, 1.0) ;
			         boundaryVolumeTint = max(boundaryVolumeTint, transmission * opticalVolumeGate * boundaryTextureCarrier * texturedBoundaryGate * 0.52) ;
	         float pathLenScale = mix(1.0, 0.46, neutralDispersionGlass) ;
         float rawPathLen = max((thickness * mix(1.0, 1.08, refractionFactor) + diffuseTransmission * 0.05) * pathLenScale, 0.0001) ;
         float shortNeutralVolume = smoothstep(0.70, 0.96, attenuationLum) * (1.0 - clamp(attenuationChroma * 8.0, 0.0, 1.0)) * (1.0 - smoothstep(0.06, 0.22, transDist)) ;
         float transmissionVolumeGate = clamp(transmission * opticalVolumeGate * (1.0 - metallic), 0.0, 1.0) ;
         float volumePathCap = max(transDist * mix(3.25, 4.95, max(tintChromaWeight, neutralDispersionGlass * 0.75)), 0.018) ;
         float cappedPathLen = min(rawPathLen, volumePathCap) ;
         float pathLen = mix(rawPathLen, cappedPathLen, transmissionVolumeGate) ;
         pathLen = mix(pathLen, min(pathLen, max(transDist * 1.85, 0.0025)), shortNeutralVolume) ;
	         vec3 transTint = exp(-sigmaA * pathLen) ;
	         if(dispersion > 0.001){
	            transTint.r *= 1.0 + dispersion * mix(0.040, 0.018, neutralDispersionGlass) ;
	            transTint.b *= 1.0 - dispersion * mix(0.026, 0.010, neutralDispersionGlass) ;
	         }
	         vec3 opticalTintBase = mix(boundaryTint, attenuationColor, attenuationPresence) ;
	         vec3 warmTexturedTint = mix(
	            max(opticalTintBase * vec3(0.95, 0.50, 0.18), vec3(0.24, 0.075, 0.018)),
	            vec3(0.70, 0.28, 0.055),
	            texturedBoundaryGate * clamp(boundaryVolumeTint, 0.0, 1.0)
	         ) ;
	         vec3 texturedVolumeTint = mix(opticalTintBase, warmTexturedTint, clamp(boundaryVolumeTint * volumeScatter * 1.35, 0.0, 0.95)) ;
         float opticalIri = clamp(iridescence, 0.0, 1.0) ;
         float softTransmissiveFilm = clamp(transmission * smoothstep(0.78, 0.98, attenuationLum) * (1.0 - clamp(attenuationChroma * 6.0, 0.0, 1.0)), 0.0, 1.0) ;
         float filmStrengthScale = mix(1.0, 0.66, softTransmissiveFilm) ;
         vec3 opticalFilmTint = vec3(1.0) ;
         if(opticalIri > 0.001){
            float filmThicknessNm = mix(iridescenceThicknessMin, iridescenceThicknessMax, clamp(iridescenceThicknessNorm, 0.0, 1.0)) ;
            float filmEta = max(iridescenceIor, 1.001) ;
            float filmCos = sqrt(max(1.0 - max(1.0 - NdotV * NdotV, 0.0) / max(filmEta * filmEta, 1.000001), 0.0)) ;
            vec3 filmWave = vec3(650.0, 510.0, 475.0) ;
            vec3 filmPhase = (4.0 * PI * filmEta * filmThicknessNm * filmCos) / filmWave ;
            vec3 filmFringe = 0.5 - 0.5 * cos(filmPhase) ;
            opticalFilmTint = mix(vec3(1.0), pow(filmFringe, vec3(0.72)), clamp(0.35 + opticalIri * 0.55 + volumeScatter * 0.20, 0.0, 0.95) * filmStrengthScale) ;
            float filmGreenBand = smoothstep(455.0, 520.0, filmThicknessNm) * (1.0 - smoothstep(610.0, 760.0, filmThicknessNm)) ;
            float filmVioletBand = 1.0 - smoothstep(430.0, 510.0, filmThicknessNm) ;
            vec3 thicknessTint = mix(vec3(0.74, 0.50, 1.18), vec3(0.12, 0.82, 0.58), filmGreenBand) ;
            thicknessTint = mix(thicknessTint, vec3(0.88, 0.54, 1.10), filmVioletBand * 0.55) ;
            opticalFilmTint = mix(opticalFilmTint, thicknessTint, clamp(opticalIri * opticalVolumeGate * (0.18 + filmGreenBand * 0.48 + volumeScatter * 0.16), 0.0, 0.72) * filmStrengthScale) ;
         }
         vec3 refrDir = refract(-V, N, 1.0 / safeIor) ;
         if(dot(refrDir, refrDir) < 1e-5){
            refrDir = -R ;
         }
         refrDir = normalize(refrDir) ;
         float refrScale = (18.0 + 88.0 * refractionFactor + 78.0 * iorNorm + 10.0 * dispersion) * max(0.10, 1.0 - roughScatter * 0.45) ;
         vec2 refrPx = refrDir.xy * refrScale ;
         vec3 sceneThrough = sampleSceneColor(refrPx) ;
         vec3 sceneBehind = sampleSceneColor(vec2(0.0)) ;
         vec3 refrEnv = sampleEnvSpec(refrDir, roughScatter) ;
         float clearTransmissionGlass = clamp(transmission * (1.0 - roughScatter) * (1.0 - tintChromaWeight) * (1.0 - metallic), 0.0, 1.0) ;
         float clearNeutralGlass = clamp(
            clearTransmissionGlass * opticalVolumeGate * (1.0 - attenuationPresence) *
            (1.0 - boundaryVolumeTint) * (1.0 - neutralDispersionGlass),
            0.0,
            1.0
         ) ;
         if(clearTransmissionGlass > 0.001){
            refrEnv = mix(
               refrEnv,
               opticalLightRigEnv(refrDir, roughScatter),
               clamp(clearTransmissionGlass * (0.18 + 0.30 * max(refractionFactor, neutralDispersionGlass)), 0.0, 0.58)
            ) ;
         }
         if(opticalIri > 0.001){
            refrEnv = mix(refrEnv, opticalLightRigEnv(refrDir, roughScatter), clamp(opticalIri * opticalVolumeGate * (0.66 + 0.26 * (1.0 - roughScatter)), 0.0, 0.92)) ;
         }
         if(dispersion > 0.001){
            float dispGate = clamp(dispersion * 0.12, 0.0, 1.0) ;
            float sceneContrast = 1.0 + dispGate * (0.20 + 0.12 * iorNorm) ;
            sceneThrough = clamp((sceneThrough - vec3(0.5)) * sceneContrast + vec3(0.5), vec3(0.0), vec3(1.0)) ;
            float dispIor = dispersion * mix(0.024, 0.052, neutralDispersionGlass) ;
            vec3 refrR = refract(-V, N, 1.0 / max(safeIor + dispIor, 1.001)) ;
            vec3 refrG = refrDir ;
            vec3 refrB = refract(-V, N, 1.0 / max(safeIor - dispIor, 1.001)) ;
            if(dot(refrR, refrR) < 1e-5){ refrR = refrDir ; }
            if(dot(refrB, refrB) < 1e-5){ refrB = refrDir ; }
            refrR = normalize(refrR) ;
            refrB = normalize(refrB) ;
            vec3 refrSampleR = sampleEnvSpec(refrR, roughScatter) ;
            vec3 refrSampleG = sampleEnvSpec(refrG, roughScatter) ;
            vec3 refrSampleB = sampleEnvSpec(refrB, roughScatter) ;
            refrEnv = vec3(refrSampleR.r, refrSampleG.g, refrSampleB.b) ;
            vec2 chromaAxis = normalize(refrDir.xy + vec2(0.001, -0.001)) ;
            float chromaPx = clamp(dispersion * (0.18 + 0.40 * neutralDispersionGlass) * (0.55 + 0.45 * iorNorm) * (1.0 - roughScatter * 0.42), 0.0, 4.5) ;
            vec2 refrPxR = refrR.xy * refrScale + chromaAxis * chromaPx ;
            vec2 refrPxG = refrG.xy * refrScale ;
            vec2 refrPxB = refrB.xy * refrScale - chromaAxis * chromaPx ;
            vec3 sceneR = sampleSceneColor(refrPxR) ;
            vec3 sceneG = sampleSceneColor(refrPxG) ;
            vec3 sceneB = sampleSceneColor(refrPxB) ;
            sceneThrough = vec3(sceneR.r, sceneG.g, sceneB.b) ;
            sceneThrough = clamp((sceneThrough - vec3(0.5)) * sceneContrast + vec3(0.5), vec3(0.0), vec3(1.0)) ;
         }
         vec3 refrDiffuse = sampleEnvDiffuse(refrDir) ;
         float fresnelView = pow(1.0 - NdotV, 5.0) ;
         vec3 opticalLo = vec3(0.0) ;
         vec3 remainLo = surfaceLo ;
         float sceneClearTransmission = 0.0 ;
         vec3 volumeTint = vec3(1.0) ;
         if(transmission > 0.001){
            float blurMix = clamp(roughScatter * 0.78 + volumeScatter * 0.44, 0.0, 1.0) ;
            vec3 throughEnv = mix(refrEnv, refrDiffuse, blurMix) ;
            vec3 throughScene = mix(sceneThrough, sceneBehind, clamp(roughScatter * 0.025 + volumeScatter * 0.010, 0.0, 0.040)) ;
            vec3 through = hasSceneColorCapture() ? throughScene : throughEnv ;
            if(hasSceneColorCapture()){
               float sceneLum = dot(throughScene, vec3(0.299, 0.587, 0.114)) ;
               float sceneFallback = 1.0 - clamp(sceneLum * 2.0, 0.0, 1.0) ;
	               float envMix = clamp(
	                  roughScatter * 0.018
	                     + volumeScatter * 0.30
	                     + tintChromaWeight * 0.20
	                     + neutralDispersionGlass * 0.12
	                     + clearTransmissionGlass * (0.095 + 0.155 * sceneFallback),
	                  0.0,
		                  0.58
		               ) ;
	               envMix *= mix(1.0, 0.42, amberVolumeCue) ;
	               through = mix(through, throughEnv, envMix) ;
	            }
            if(neutralDispersionGlass > 0.001){
               float neutralLift = clamp(neutralDispersionGlass * (0.16 + 0.10 * (1.0 - roughScatter)), 0.0, 0.24) ;
               through = mix(through, through * 1.10 + vec3(0.012, 0.014, 0.018), neutralLift) ;
            }
            if(opticalIri > 0.001){
               through = mix(through, throughEnv * opticalFilmTint, clamp(opticalIri * opticalVolumeGate * (0.44 + volumeScatter * 0.50 + fresnelView * 0.56), 0.0, 0.90)) ;
	            }
		            through *= transTint * thinTransmissionTint ;
			            if(amberVolumeCue > 0.001){
			               vec3 amberFilteredThrough = pow(max(through, vec3(0.0)), vec3(1.34)) * vec3(0.50, 0.155, 0.030) ;
			               through = mix(through, amberFilteredThrough, clamp(amberVolumeCue * (0.76 + 0.22 * volumeScatter) * (1.0 - roughScatter * 0.18), 0.0, 0.94)) ;
			            }
	            float thinColoredGlass = clamp(
	               thinTintGate * (1.0 - opticalVolumeGate * 0.30) * (boundaryChroma * 3.40 + boundaryLum * 0.18),
	               0.0,
	               1.0
	            ) ;
	            float coloredGlassBody = clamp(
	               transmission * (1.0 - metallic) * (thinColoredGlass + boundaryChroma * 0.75 + boundaryVolumeTint * 0.55) * (0.68 + 0.50 * volumeScatter) * (1.0 - attenuationPresence * 0.28),
	               0.0,
	               1.0
	            ) ;
	            if(coloredGlassBody > 0.001){
	               vec3 coloredGlassFill = sqrt(max(boundaryTint, vec3(0.0))) * (0.220 + 0.320 * (1.0 - roughScatter)) * (0.55 + 0.45 * viewWrap) ;
	               through = max(through, coloredGlassFill * coloredGlassBody) ;
	            }
	            vec3 transmitWeight = clamp(transmission * (1.0 - metallic), 0.0, 1.0) * clamp(vec3(1.0) - fresnelV * 0.70, vec3(0.0), vec3(1.0)) ;
            transmitWeight *= mix(vec3(1.0), vec3(0.52), clamp(opticalIri * opticalVolumeGate * (0.46 + volumeScatter * 0.46), 0.0, 0.78) * filmStrengthScale) ;
	            sceneClearTransmission = hasSceneColorCapture() ? clamp(transmission * (1.0 - metallic) * (1.0 - roughScatter * 0.70), 0.0, 1.0) : 0.0 ;
	            float volumeTintWeight = mix(0.18, 0.72, volumeScatter) * attenuationPresence * max(tintChromaWeight, neutralDispersionGlass * 1.35) ;
		            volumeTintWeight = max(volumeTintWeight, boundaryVolumeTint * mix(0.58, 0.92, volumeScatter)) ;
		            volumeTint = mix(vec3(1.0), opticalTintBase, clamp(volumeTintWeight, 0.0, 0.92)) ;
		            volumeTint = mix(volumeTint, texturedVolumeTint, clamp(boundaryVolumeTint * (0.55 + 0.35 * volumeScatter), 0.0, 0.85)) ;
	            volumeTint = mix(volumeTint, opticalFilmTint, clamp(opticalIri * opticalVolumeGate * (0.42 + volumeScatter * 0.52 + fresnelView * 0.50), 0.0, 0.90)) ;
            float denseVolumeCue = max(attenuationPresence * tintChromaWeight, neutralDispersionGlass * 0.78) ;
            denseVolumeCue = max(denseVolumeCue, attenuationPresence * neutralDispersionGlass * 0.40) ;
            float denseVolumeTint = clamp(transmission * opticalVolumeGate * denseVolumeCue, 0.0, 1.0) ;
	            vec3 transmitted = through * volumeTint * transmitWeight * mix(1.00, 0.78, roughScatter) * mix(1.0, 1.08, sceneClearTransmission) * mix(1.0, 0.66, opticalIri * opticalVolumeGate) * mix(1.0, mix(0.48, 0.36, amberVolumeCue), denseVolumeTint) ;
            opticalLo += transmitted ;
		            float bodyDensity = clamp(roughScatter * 0.40 + volumeScatter * 0.45 + max(tintChromaWeight, boundaryVolumeTint * 1.08) * 0.55, 0.0, 1.0) ;
            float opaqueLoss = mix(0.995, 0.92, bodyDensity) ;
            remainLo *= clamp(vec3(1.0) - transmitWeight * opaqueLoss, vec3(0.0), vec3(1.0)) ;
	            remainLo *= mix(vec3(1.0), vec3(0.020 + 0.22 * fresnelView + 0.10 * roughScatter + 0.055 * tintChromaWeight), sceneClearTransmission) ;
		            remainLo += surfaceLo * clamp(transmission * (1.0 - metallic) * (0.10 + 0.22 * fresnelMaxV) * (1.0 - roughScatter * 0.70), 0.0, 0.32) * mix(1.0, mix(0.14, 0.44, amberVolumeCue), denseVolumeTint) ;
         }
         if(dtMix > 0.001){
            vec3 dtEnv = envDiffuseBack ;
            vec3 dtScene = sceneBehind ;
            // Diffuse transmission is a broad BTDF/scatter term, not a clear
            // refraction lookup. In gallery captures the scene-color buffer is
            // mostly black background, so using it as the primary light source
            // crushes thin translucent assets to near-black.
            vec3 dtBase = dtEnv ;
            if(hasSceneColorCapture()){
               dtBase = mix(dtEnv, dtScene, clamp(roughScatter * 0.025 + volumeScatter * 0.02, 0.0, 0.06)) ;
            }
            float dtColoredVolume = clamp(attenuationPresence * volumeScatter, 0.0, 1.0) ;
            vec3 dtVolumeAlbedo = mix(diffuseTransmissionScatterAlbedo, diffuseTransmissionScatterAlbedo * opticalTintBase, clamp(0.30 + 0.55 * volumeScatter, 0.0, 0.90)) ;
            vec3 dtWeight = mix(diffuseTransmissionScatterAlbedo, dtVolumeAlbedo, dtColoredVolume) * (1.0 - metallic) * (1.0 - transmission) * dtMix * max(1.0 - fresnelMaxV, 0.0) ;
            float dtAlphaCardLift = diffuseTransmissionAlphaCard ? 6.00 : 1.0 ;
            vec3 dtThrough = dtBase * dtWeight * (0.22 + 0.48 * viewWrap) * dtAlphaCardLift ;
            opticalLo += dtThrough * mix(vec3(1.0), transTint, mix(0.25, 0.78, dtColoredVolume)) ;
            if(diffuseTransmissionAlphaCard){
               opticalLo += diffuseTransmissionCardFill * dtMix * (0.60 + 0.55 * viewWrap) * (1.0 - metallic) ;
            }
            float dtOpaqueLoss = mix(0.015 + 0.025 * viewWrap, 0.82 + 0.12 * viewWrap, dtColoredVolume) ;
            vec3 dtBodyTint = mix(opticalTintBase, diffuseTransmissionColor, clamp(0.72 + 0.18 * volumeScatter, 0.0, 0.96)) ;
            remainLo *= 1.0 - clamp(dtMix * dtOpaqueLoss, 0.0, 0.94) ;
            remainLo *= mix(vec3(1.0), dtBodyTint, clamp(dtColoredVolume * 0.82, 0.0, 0.94)) ;
         }
         if(subsurfaceFactor > 0.001){
            float sssColoredVolume = clamp(attenuationPresence * volumeScatter * max(diffuseTransmission, transmission), 0.0, 1.0) ;
            vec3 subsurfaceTint = mix(vec3(1.0), attenuationColor * diffuseTransmissionScatterAlbedo, 0.5) ;
            subsurfaceTint = mix(subsurfaceTint, diffuseTransmissionColor, clamp(sssColoredVolume * 0.68, 0.0, 0.82)) ;
            opticalLo += refrDiffuse * subsurfaceTint * subsurfaceFactor * (1.0 - metallic) * mix(0.06, 0.24, volumeScatter + dtMix * 0.24) * mix(1.0, 2.15, sssColoredVolume) ;
         }
         float fresnelEdge = pow5(1.0 - NdotV) ;
         vec3 edgeReflect = envSpec * mix(0.050, 0.72, fresnelEdge) * mix(0.18, 0.66, fresnelMaxV) ;
         edgeReflect += refrEnv * fresnelV * mix(0.12, 0.035, roughScatter) ;
         if(hasSceneColorCapture()){
            float reflectPx = (24.0 + 108.0 * max(refractionFactor, iorNorm) + 16.0 * dispersion) * max(0.16, 1.0 - roughScatter * 0.62) ;
            vec3 sceneReflect = sampleSceneColor(R.xy * reflectPx) ;
            sceneReflect = clamp((sceneReflect - vec3(0.5)) * (1.05 + 0.25 * volumeScatter) + vec3(0.5), vec3(0.0), vec3(1.0)) ;
            float sceneReflectMix = clamp(transmission * (1.0 - metallic) * (1.0 - roughScatter) * (0.18 + 0.58 * fresnelEdge + 0.30 * volumeScatter + 0.26 * tintChromaWeight), 0.0, 0.86) ;
            vec3 sceneReflectTint = mix(sceneReflect, sceneReflect * mix(vec3(1.0), attenuationColor, 0.42), clamp(attenuationPresence + boundaryVolumeTint, 0.0, 1.0)) ;
            edgeReflect = mix(edgeReflect, sceneReflectTint + edgeReflect * 0.35, sceneReflectMix) ;
         }
         float coloredSpecGlass = clamp(transmission * opticalVolumeGate * max(tintChromaWeight, attenuationPresence * 0.45) * (1.0 - roughScatter), 0.0, 1.0) ;
         if(coloredSpecGlass > 0.001){
            vec3 coloredSpecEnv = mix(envSpec, opticalLightRigEnv(R, roughScatter), 0.58) ;
            edgeReflect += coloredSpecEnv * mix(vec3(1.0), attenuationColor, 0.28) * coloredSpecGlass * (0.24 + 0.76 * fresnelEdge) ;
         }
         if(clearTransmissionGlass > 0.001){
            edgeReflect += opticalLightRigEnv(R, roughScatter) * clearTransmissionGlass * (0.060 + 0.46 * fresnelEdge) * (1.0 - roughScatter * 0.55) ;
            edgeReflect += refrEnv * clearTransmissionGlass * (0.038 + 0.090 * neutralDispersionGlass + 0.13 * fresnelEdge) * (1.0 - roughScatter * 0.45) ;
         }
         edgeReflect *= mix(vec3(1.0), mix(thinTransmissionTint, vec3(0.35), 0.35), clamp(darkThinTint * 0.70, 0.0, 0.70)) ;
         if(opticalIri > 0.001){
            edgeReflect *= mix(vec3(1.0), opticalFilmTint, clamp(opticalIri * (0.26 + 0.58 * opticalVolumeGate), 0.0, 0.82)) ;
            edgeReflect += envSpec * opticalFilmTint * opticalIri * opticalVolumeGate * (0.17 + 0.56 * fresnelEdge) ;
            edgeReflect += refrEnv * opticalFilmTint * opticalIri * opticalVolumeGate * (0.11 + 0.36 * fresnelEdge) ;
         }
         if(transmission <= 0.001 && diffuseTransmission > 0.001){
            edgeReflect *= 0.18 ;
         }
         opticalLo += edgeReflect ;
         if(transmission > 0.001){
            float silhouette = pow(sat1(1.0 - NdotV), 1.35) ;
	            vec3 bodyTint = mix(opticalTintBase, attenuationColor, clamp((0.24 + volumeScatter * 0.46) * attenuationPresence, 0.0, 1.0)) ;
	            bodyTint = mix(bodyTint, texturedVolumeTint, clamp(boundaryVolumeTint * 0.78, 0.0, 0.78)) ;
            bodyTint = mix(bodyTint, vec3(1.0), clamp(dispersion * 0.018, 0.0, 0.10) * tintChromaWeight) ;
            bodyTint = mix(bodyTint, opticalFilmTint, clamp(opticalIri * opticalVolumeGate * (0.30 + volumeScatter * 0.54 + silhouette * 0.42), 0.0, 0.88)) ;
            vec3 bodyLight = mix(refrEnv, envSpec, 0.62) * bodyTint ;
            vec3 bodyCore = mix(bodyTint, bodyLight, 0.78) ;
		            float coloredVolumeBody = max(attenuationPresence * max(tintChromaWeight, neutralDispersionGlass * 0.72), boundaryVolumeTint * 1.04) ;
            float bodyStrength = max(neutralDispersionGlass * 0.060, mix(0.014, 0.145, tintChromaWeight)) ;
            bodyStrength = max(bodyStrength, coloredVolumeBody * mix(0.045, 0.180, volumeScatter)) ;
            bodyStrength = max(bodyStrength, opticalIri * opticalVolumeGate * mix(0.045, 0.18, clamp(volumeScatter + silhouette * 0.75, 0.0, 1.0))) ;
            opticalLo += bodyCore * transmission * bodyStrength * (0.12 + 1.20 * silhouette) * (0.30 + 0.50 * volumeScatter) ;
            if(dispersion > 0.001){
               vec3 fringe = mix(vec3(0.52, 0.20, 0.62), vec3(0.30, 0.72, 1.16), neutralDispersionGlass * 0.45) * clamp(dispersion / 5.0, 0.0, 1.0) ;
               float fringeStrength = max(mix(0.035, 0.34, tintChromaWeight), neutralDispersionGlass * 0.12) ;
               opticalLo += fringe * transmission * fringeStrength * (0.025 + 0.22 * silhouette) * (0.38 + 0.42 * iorNorm) ;
            }
         }
         Lo = max(remainLo, vec3(0.0)) + opticalLo ;
         if(transmission > 0.001){
		            float denseVolumeCue = max(max(attenuationPresence * max(tintChromaWeight, 0.42 * volumeScatter), boundaryVolumeTint * 1.15), neutralDispersionGlass * 0.86) ;
            denseVolumeCue = max(denseVolumeCue, attenuationPresence * neutralDispersionGlass * 0.46) ;
            float coloredVolumeGate = clamp(transmission * opticalVolumeGate * denseVolumeCue, 0.0, 1.0) ;
	            if(coloredVolumeGate > 0.001){
	               vec3 chromaDenseTint = mix(texturedVolumeTint, attenuationColor * vec3(0.95, 0.78, 0.55), clamp(tintChromaWeight, 0.0, 1.0)) ;
	               vec3 denseBodyTint = mix(chromaDenseTint, vec3(0.22, 0.31, 0.37), neutralDispersionGlass) ;
		               float amberVolume = amberVolumeCue ;
		               denseBodyTint = mix(denseBodyTint, vec3(0.88, 0.32, 0.060), amberVolume * 0.80) ;
		               vec3 denseBodyColor = denseBodyTint * mix(0.18, 0.68, 1.0 - roughScatter) * mix(0.46, 0.96, volumeScatter) ;
	               denseBodyColor += edgeReflect * (0.05 + 0.17 * (1.0 - roughScatter)) ;
	               vec3 tintedLo = max(Lo * mix(vec3(1.0), denseBodyTint, 0.93), denseBodyColor) ;
		               Lo = mix(Lo, tintedLo, clamp(coloredVolumeGate * mix(0.34 + 0.42 * volumeScatter, 0.22 + 0.30 * volumeScatter, amberVolume), 0.0, mix(0.80, 0.58, amberVolume))) ;
		               float denseShade = clamp(coloredVolumeGate * mix(0.18 + 0.40 * volumeScatter, 0.08 + 0.22 * volumeScatter, amberVolume) * (1.0 - roughScatter * 0.22), 0.0, mix(0.62, 0.32, amberVolume)) ;
		               Lo *= mix(vec3(1.0), max(denseBodyTint * mix(0.80, 1.0, neutralDispersionGlass), vec3(0.075, 0.026, 0.006)), denseShade) ;
	               if(texturedBoundaryGate > 0.5 && attenuationColor.r < 0.92 && attenuationColor.g < 0.58 && attenuationColor.b < 0.22){
		                  vec3 amberBody = mix(texturedVolumeTint, vec3(0.72, 0.33, 0.090), 0.74) ;
		                  float amberDensity = clamp(boundaryVolumeTint * volumeScatter * (0.54 + 0.28 * (1.0 - roughScatter)), 0.0, 0.48) ;
		                  Lo = mix(Lo, max(Lo * mix(amberBody, vec3(1.0), 0.28), amberBody * 0.090), amberDensity) ;
	               }
	            }
            float clearVolumeBody = clamp(transmission * opticalVolumeGate * (1.0 - attenuationPresence) * (1.0 - tintChromaWeight) * (1.0 - neutralDispersionGlass), 0.0, 1.0) ;
            if(clearVolumeBody > 0.001){
               float clearBodyShade = clamp(clearVolumeBody * (0.030 + 0.070 * iorNorm) * (1.0 - roughScatter * 0.45), 0.0, 0.11) ;
               Lo *= mix(vec3(1.0), vec3(0.72, 0.77, 0.84), clearBodyShade) ;
            }
         }
			         float amberAttenuationGlass = clamp(transmission * opticalVolumeGate * attenuationPresence * tintChromaWeight * smoothstep(0.52, 0.96, attenuationColor.r) * (1.0 - smoothstep(0.14, 0.38, attenuationColor.b)), 0.0, 1.0) ;
			         float amberColorGlass = amberAttenuationGlass ;
		         float amberAttenuationCue = clamp(
		            transmission * smoothstep(0.35, 1.35, thickness) *
		            (1.0 - smoothstep(0.16, 0.42, attenuationColor.b)),
		            0.0,
		            1.0
		         ) ;
		         float amberBoundaryCue = clamp(
		            transmission * smoothstep(0.20, 1.20, thickness) * texturedBoundaryGate *
		            smoothstep(0.10, 0.55, boundaryLum) *
		            clamp((boundaryTint.r - boundaryTint.b) * 2.8 + (boundaryTint.g - boundaryTint.b) * 1.4 + boundaryChroma * 0.65, 0.0, 1.0),
		            0.0,
		            1.0
		         ) ;
		         amberColorGlass = max(amberColorGlass, max(amberAttenuationCue, amberBoundaryCue)) ;
	         if(transmission > 0.001 && sceneClearTransmission > 0.001){
	            vec3 refractedSceneLo = sceneThrough * transTint * thinTransmissionTint * volumeTint * mix(1.02, 0.76, roughScatter) ;
		            vec3 rimLo = edgeReflect * (0.10 + 0.72 * fresnelEdge) ;
		            if(amberColorGlass > 0.001){
				               float amberSceneLum = dot(sceneThrough, vec3(0.2126, 0.7152, 0.0722)) ;
				               vec3 filteredScene = mix(vec3(amberSceneLum), pow(max(sceneThrough, vec3(0.0)), vec3(1.42)), 0.30) ;
				               vec3 amberThrough = filteredScene * vec3(0.64, 0.215, 0.042) * (0.54 + 0.26 * fresnelEdge) ;
				               vec3 amberRim = edgeReflect * vec3(1.38, 0.58, 0.16) * (0.060 + 0.92 * fresnelEdge) ;
			               refractedSceneLo = mix(refractedSceneLo, amberThrough + amberRim, amberColorGlass * (0.82 + 0.14 * volumeScatter) * (1.0 - roughScatter * 0.42)) ;
		            }
	            float thinColoredSceneTint = clamp(
	               thinTintGate * (1.0 - opticalVolumeGate * 0.35) * (boundaryChroma * 3.80 + boundaryLum * 0.22),
	               0.0,
	               1.0
	            ) ;
	            if(thinColoredSceneTint > 0.001){
	               vec3 thinSceneColor = sqrt(max(boundaryTint, vec3(0.0))) * (0.42 + 0.42 * (1.0 - roughScatter)) * (0.80 + 0.20 * viewWrap) ;
	               refractedSceneLo = max(refractedSceneLo, thinSceneColor * clamp(thinColoredSceneTint * 1.15, 0.0, 1.0) * transmission * (1.0 - metallic)) ;
	            }
            float sceneResolve = sceneClearTransmission * mix(0.64, 0.96, 1.0 - roughScatter) ;
	            sceneResolve *= mix(1.0, 0.52, thinColoredSceneTint) ;
            float clearVolumeDensity = clamp(transmission * opticalVolumeGate * (1.0 - attenuationPresence) * (1.0 - tintChromaWeight) * (1.0 - neutralDispersionGlass), 0.0, 1.0) ;
            sceneResolve *= mix(1.0, 0.86, clearVolumeDensity * (1.0 - roughScatter * 0.50)) ;
				            float denseSceneLoss = mix(0.80, 0.56, clamp(attenuationPresence + neutralDispersionGlass + tintChromaWeight + boundaryVolumeTint * 1.05, 0.0, 1.0)) ;
				            sceneResolve *= mix(1.0, denseSceneLoss, volumeScatter) ;
				            sceneResolve *= mix(1.0, 0.78, clamp(boundaryVolumeTint * volumeScatter, 0.0, 1.0)) ;
				            sceneResolve *= mix(1.0, 0.92, clamp(tintChromaWeight + neutralDispersionGlass, 0.0, 1.0)) ;
				            sceneResolve *= mix(1.0, 0.72, neutralDispersionGlass * (1.0 - roughScatter * 0.28)) ;
					            sceneResolve = mix(sceneResolve, min(sceneResolve, mix(0.14, 0.22, volumeScatter)), amberColorGlass * (1.0 - roughScatter * 0.26)) ;
				            Lo = mix(Lo, refractedSceneLo + rimLo, clamp(sceneResolve, 0.0, 0.96)) ;
		         }
			         if(amberColorGlass > 0.001){
			            vec3 sceneWarp = pow(max(sceneThrough, vec3(0.0)), vec3(1.25)) ;
			            float sceneLum = dot(sceneWarp, vec3(0.2126, 0.7152, 0.0722)) ;
				            float checkerShade = smoothstep(0.30, 0.92, sceneLum) * 0.82 ;
					            vec3 amberDark = vec3(0.095, 0.030, 0.0060) ;
					            vec3 amberMid = vec3(0.46, 0.145, 0.026) ;
					            vec3 amberHot = vec3(1.02, 0.36, 0.075) ;
			            vec3 amberFinalThrough = mix(amberDark, amberMid, checkerShade) ;
			            amberFinalThrough = mix(amberFinalThrough, amberHot, pow(fresnelEdge, 1.70) * 0.42) ;
			            if(boundaryVolumeTint > 0.001){
			               float amberMottle = clamp(boundaryVolumeTint * (boundaryLum * 0.78 + boundaryChroma * 1.10), 0.0, 1.0) ;
			               vec3 mottleColor = mix(vec3(0.30, 0.070, 0.010), vec3(0.92, 0.30, 0.050), amberMottle) ;
			               amberFinalThrough = mix(amberFinalThrough, amberFinalThrough * mottleColor * 1.18, clamp(boundaryVolumeTint * 0.58, 0.0, 0.76)) ;
			            }
				            vec3 amberFinalGloss = edgeReflect * vec3(1.20, 0.46, 0.12) * (0.020 + 0.62 * fresnelEdge) ;
					            vec3 amberFinal = amberFinalThrough * (0.68 + 0.18 * viewWrap) + amberFinalGloss ;
					            Lo = mix(Lo, amberFinal, clamp(amberColorGlass * (0.36 + 0.18 * volumeScatter) * (1.0 - roughScatter * 0.45), 0.0, 0.62)) ;
			         }
		         if(boundaryVolumeTint > 0.001){
			            float texturedVolumeResolve = clamp(boundaryVolumeTint * volumeScatter * (0.24 + 0.10 * roughScatter), 0.0, 0.28) ;
			            vec3 amberBody = vec3(0.38, 0.16, 0.036) * (0.90 + 0.35 * (1.0 - roughScatter)) ;
			            vec3 softenedTint = mix(texturedVolumeTint, vec3(1.0), 0.35) ;
			            Lo = mix(Lo, max(Lo * softenedTint, amberBody * 0.42), texturedVolumeResolve) ;
		            float mottle = clamp(boundaryLum * 0.95 + boundaryChroma * 0.80, 0.0, 1.0) ;
		            vec3 mottleTint = mix(vec3(0.42, 0.18, 0.045), vec3(1.05, 0.62, 0.22), mottle) ;
			            Lo *= mix(vec3(1.0), mottleTint, clamp(boundaryVolumeTint * 0.20, 0.0, 0.24)) ;
		         }
		         if(amberColorGlass > 0.001){
		            vec3 postScene = pow(max(sceneThrough, vec3(0.0)), vec3(1.35)) ;
		            float postLum = dot(postScene, vec3(0.2126, 0.7152, 0.0722)) ;
		            float postCheck = smoothstep(0.30, 0.94, postLum) ;
			            vec3 postAmber = mix(vec3(0.080, 0.025, 0.0050), vec3(0.56, 0.175, 0.030), postCheck * 0.82) ;
			            vec3 postThroughAmber = pow(max(sceneThrough, vec3(0.0)), vec3(1.05)) * vec3(0.86, 0.32, 0.070) ;
			            postAmber = mix(postAmber, postThroughAmber, clamp(0.10 + postCheck * 0.10, 0.0, 0.24)) ;
		            float amberRimPower = pow(fresnelEdge, 2.35) ;
		            postAmber = mix(postAmber, vec3(0.64, 0.18, 0.028), amberRimPower * 0.16) ;
		            if(boundaryVolumeTint > 0.001){
		               float postMottle = clamp(boundaryVolumeTint * (boundaryLum * 0.70 + boundaryChroma * 1.15), 0.0, 1.0) ;
		               postAmber = mix(postAmber, postAmber * mix(vec3(0.42, 0.105, 0.018), vec3(0.72, 0.21, 0.034), postMottle), clamp(boundaryVolumeTint * 0.24, 0.0, 0.36)) ;
		            }
		            vec3 postGloss = edgeReflect * vec3(1.25, 0.46, 0.10) * (0.018 + 0.62 * amberRimPower) ;
			            vec3 postFinal = postAmber * (0.72 + 0.16 * viewWrap) + postGloss ;
			            Lo = mix(Lo, postFinal, clamp(amberColorGlass * (0.22 + 0.16 * volumeScatter) * (1.0 - roughScatter * 0.36), 0.0, 0.36)) ;
		         }
				         if(amberAttenuationGlass > 0.001 && hasSceneColorCapture()){
				            float denseAmber = smoothstep(0.45, 0.95, amberAttenuationGlass) ;
			            float amberSceneLum = dot(sceneThrough, vec3(0.2126, 0.7152, 0.0722)) ;
			            vec3 sceneHi = clamp((sceneThrough - vec3(0.40)) * mix(1.35, 1.95, denseAmber) + vec3(0.42), vec3(0.0), vec3(1.0)) ;
			            float amberChecker = smoothstep(mix(0.34, 0.30, denseAmber), mix(0.78, 0.68, denseAmber), amberSceneLum) ;
				            vec3 amberDark = mix(vec3(0.075, 0.0240, 0.0050), vec3(0.060, 0.0180, 0.0036), denseAmber) ;
				            vec3 amberMid = mix(vec3(0.50, 0.160, 0.030), vec3(0.40, 0.125, 0.0220), denseAmber) ;
			            vec3 amberScene = mix(amberDark, amberMid, amberChecker) ;
				            vec3 checkerRefract = sceneHi * mix(vec3(0.62, 0.240, 0.055), vec3(0.52, 0.190, 0.040), denseAmber) ;
				            amberScene = mix(amberScene, checkerRefract, mix(0.10, 0.20, denseAmber)) ;
			            vec3 amberCore = mix(vec3(0.025, 0.0060, 0.0010), vec3(0.055, 0.0140, 0.0024), amberChecker) ;
			            amberCore = mix(amberCore, amberCore * vec3(0.72, 0.58, 0.46), denseAmber) ;
			            vec3 tightSpec = max(edgeReflect - vec3(0.18), vec3(0.0)) ;
			            tightSpec = tightSpec * tightSpec * vec3(2.2, 1.45, 0.70) ;
			            vec3 amberSpec = tightSpec * vec3(1.15, 0.43, 0.085) * (0.045 + 1.18 * pow(fresnelEdge, 1.55)) * (1.0 - roughScatter * 0.40) ;
			            vec3 amberTarget = amberScene * mix(0.82 + 0.16 * viewWrap, 0.70 + 0.12 * viewWrap, denseAmber) + amberCore * (0.05 + 0.09 * fresnelEdge) + amberSpec ;
				            float amberResolve = clamp(amberAttenuationGlass * mix(0.42, 0.62, denseAmber) * (1.0 - roughScatter * 0.20), 0.0, 0.66) ;
					            vec3 amberBaseLoss = mix(vec3(0.74, 0.460, 0.200), vec3(0.58, 0.300, 0.110), denseAmber) ;
				            Lo = mix(Lo * amberBaseLoss, amberTarget, amberResolve) ;
			            if(denseAmber > 0.001){
			               vec3 denseScene = mix(vec3(0.035, 0.0140, 0.0050), vec3(0.125, 0.064, 0.028), amberChecker) ;
			               denseScene = mix(denseScene, sceneHi * vec3(0.135, 0.073, 0.036), 0.42) ;
			               vec3 denseGloss = tightSpec * vec3(0.60, 0.40, 0.22) * (0.045 + 1.10 * pow(fresnelEdge, 1.35)) ;
			               vec3 denseTarget = denseScene * (0.62 + 0.10 * viewWrap) + denseGloss ;
					               Lo = mix(Lo * vec3(0.72, 0.460, 0.240), denseTarget, clamp(denseAmber * 0.32, 0.0, 0.32)) ;
				            }
				         }
				         if(amberVolumeCue > 0.001){
					            vec3 amberLift = mix(
					               Lo * vec3(0.68, 0.32, 0.12),
					               vec3(0.54, 0.150, 0.026) * (0.34 + 0.36 * viewWrap) + edgeReflect * vec3(1.35, 0.48, 0.095) * (0.045 + 0.62 * fresnelEdge),
					               0.72
					            ) ;
					            Lo = mix(Lo, amberLift, clamp(amberVolumeCue * (0.50 + 0.28 * volumeScatter) * (1.0 - roughScatter * 0.18), 0.0, 0.76)) ;
				         }
					         if(amberColorGlass > 0.001){
						            float amberBodyMask = clamp(max(amberColorGlass, amberVolumeCue) * (0.78 + 0.18 * volumeScatter) * (1.0 - roughScatter * 0.10), 0.0, 0.96) ;
						            float amberSceneLum = dot(sceneThrough, vec3(0.2126, 0.7152, 0.0722)) ;
						            float amberScene = smoothstep(0.18, 0.88, amberSceneLum) ;
						            amberScene = mix(amberScene * 0.34, pow(amberScene, 1.80) * 0.68, 0.58) ;
						            vec3 amberBodyBase = mix(vec3(0.040, 0.011, 0.0018), vec3(0.38, 0.105, 0.018), amberScene) ;
						            amberBodyBase = mix(amberBodyBase, vec3(0.95, 0.35, 0.075), pow(fresnelEdge, 1.95) * 0.34 + smoothstep(0.74, 1.0, amberScene) * 0.12) ;
						            float amberThick = clamp(thickness * 0.55, 0.0, 1.0) ;
						            amberBodyBase *= mix(1.04, 0.56, amberThick * (1.0 - fresnelEdge * 0.45)) ;
						            vec3 cleanAmberSpec = mix(opticalLightRigEnv(R, max(roughScatter, 0.03)), refrEnv, 0.34) ;
						            vec3 amberBodySpec = cleanAmberSpec * vec3(1.20, 0.50, 0.12) * (0.040 + 0.96 * pow(fresnelEdge, 1.35)) * (1.0 - roughScatter * 0.35) ;
						            vec3 amberKeyDir = normalize(vec3(-0.34, 0.80, 0.46)) ;
						            vec3 amberHalf = normalize(amberKeyDir + V) ;
						            float amberKeySpec = pow(max(dot(N, amberHalf), 0.0), mix(96.0, 38.0, roughScatter)) + pow(max(dot(R, amberKeyDir), 0.0), 18.0) * 0.42 ;
						            amberBodySpec += vec3(1.55, 0.72, 0.22) * amberKeySpec * (0.20 + 0.80 * amberBodyMask) ;
						            vec3 amberBodyTarget = amberBodyBase * (0.76 + 0.16 * viewWrap) + amberBodySpec + envSpec * vec3(0.42, 0.18, 0.040) * (0.020 + 0.10 * viewWrap) ;
						            Lo = mix(Lo * vec3(0.34, 0.14, 0.045), amberBodyTarget, amberBodyMask) ;
						         }
						         if(neutralDispersionGlass > 0.001 && transmission > 0.001){
						            float neutralBodyMask = clamp(neutralDispersionGlass * transmission * opticalVolumeGate * (0.50 + 0.34 * volumeScatter) * (1.0 - roughScatter * 0.18), 0.0, 0.86) ;
						            float neutralSceneLum = dot(sceneThrough, vec3(0.2126, 0.7152, 0.0722)) ;
						            float neutralScene = smoothstep(0.14, 0.92, neutralSceneLum) ;
						            neutralScene = mix(neutralScene * 0.28, pow(neutralScene, 1.55) * 0.58, 0.55) ;
						            vec3 neutralBodyBase = mix(vec3(0.075, 0.110, 0.145), vec3(0.32, 0.44, 0.52), neutralScene + (0.24 + 0.14 * viewWrap)) ;
						            vec3 neutralBodySpec = mix(opticalLightRigEnv(R, max(roughScatter, 0.04)), refrEnv, 0.38) * vec3(0.68, 0.90, 1.10) * (0.038 + 0.58 * fresnelEdge) ;
						            vec3 neutralKeyDir = normalize(vec3(-0.30, 0.82, 0.42)) ;
						            vec3 neutralHalf = normalize(neutralKeyDir + V) ;
						            float neutralKeySpec = pow(max(dot(N, neutralHalf), 0.0), mix(112.0, 42.0, roughScatter)) + pow(max(dot(R, neutralKeyDir), 0.0), 18.0) * 0.36 ;
						            neutralBodySpec += vec3(0.95, 1.18, 1.45) * neutralKeySpec * (0.16 + 0.84 * neutralBodyMask) ;
						            vec3 neutralFringe = vec3(0.12, 0.20, 0.38) * clamp(dispersion * 0.065, 0.0, 0.24) * (0.18 + 0.82 * fresnelEdge) ;
						            vec3 neutralBodyTarget = neutralBodyBase * (0.70 + 0.18 * viewWrap) + neutralBodySpec + neutralFringe ;
						            Lo = mix(Lo * vec3(0.36, 0.46, 0.56), neutralBodyTarget, neutralBodyMask) ;
						         }
				         float transAlpha = clamp(0.02 + diffuseTransmission * 0.05, 0.02, 1.0) ;
         if(transmission <= 0.001 && diffuseTransmission > 0.001){
            // KHR_materials_diffuse_transmission is a surface/BTDF term, not
            // framebuffer transparency. Keeping the old tiny alpha made
            // teacups, oranges, and scattering assets blend almost black over
            // the gallery background.
            transAlpha = max(transAlpha, 0.94) ;
         }
         if(transmission > 0.0){
            float clarityLoss = clamp(roughScatter * 0.92 + volumeScatter * 0.80, 0.0, 1.0) ;
            float opticalDensity = clamp(roughScatter * 0.38 + volumeScatter * 0.38 + tintChromaWeight * 0.52, 0.0, 1.0) ;
		            float clearAlpha = mix(0.30, 0.46, iorNorm) ;
		            float denseAlpha = 0.88 + 0.08 * roughScatter ;
		            float targetAlpha = mix(clearAlpha, denseAlpha, opticalDensity) ;
	            float thinGlassAlphaCue = clamp(
	               thinTintGate * (1.0 - opticalVolumeGate * 0.35) * (boundaryChroma * 4.00 + boundaryLum * 0.20),
	               0.0,
	               1.0
	            ) ;
		            targetAlpha = mix(targetAlpha, max(targetAlpha, 0.78), thinGlassAlphaCue) ;
		            targetAlpha = mix(targetAlpha, targetAlpha + 0.12, clarityLoss * roughScatter) ;
			            targetAlpha = mix(targetAlpha, max(targetAlpha, 0.98), clamp(boundaryVolumeTint * volumeScatter * (1.0 - amberVolumeCue * 0.78), 0.0, 1.0)) ;
			            targetAlpha = mix(targetAlpha, min(targetAlpha, mix(0.62, 0.78, roughScatter)), amberVolumeCue * (1.0 - roughScatter * 0.28)) ;
			            targetAlpha = mix(targetAlpha, min(targetAlpha, clearAlpha), clearNeutralGlass * (1.0 - roughScatter * 0.35)) ;
		            transAlpha = max(transAlpha, clamp(targetAlpha, 0.22, 0.96)) ;
		         }
		         transAlpha *= mix(1.0, clamp(dot(transTint, vec3(0.299, 0.587, 0.114)), 0.55, 1.0), 0.06) ;
			         if(alphaMode == 2u || transmission > 0.0 || diffuseTransmission > 0.0){
			            if(hasSceneColorCapture()){
			               float captureAlpha = alphaMode == 2u ? min(baseAlpha, transAlpha) : 1.0 ;
			               if(transmission > 0.0 && baseAlpha >= 0.995){
			                  captureAlpha = 1.0 ;
			               }
			               baseAlpha = clamp(captureAlpha, 0.02, 1.0) ;
			            } else {
			               baseAlpha = min(baseAlpha, transAlpha) ;
			            }
			         }
      }
      if(baseIndex < 1024u && !extensionSurface && metallic <= 0.12 && transmission <= 0.001 && diffuseTransmission <= 0.001){
         float albedoLum = dot(baseColor, vec3(0.299, 0.587, 0.114)) ;
         float albedoChroma = max3(abs(baseColor - vec3(albedoLum))) ;
         float albedoFloor = clamp(0.18 + roughness * 0.38 + albedoChroma * 0.22, 0.22, 0.62) ;
         float litLum = dot(max(Lo, vec3(0.0)), vec3(0.299, 0.587, 0.114)) ;
         vec3 albedoTarget = baseColor * max(albedoFloor, litLum * 1.10) ;
         float lowMetalAlbedo = 1.0 - smoothstep(0.04, 0.12, metallic) ;
         vec3 albedoLit = max(albedoTarget, baseColor * max(albedoFloor, 0.42)) ;
         float albedoWeight = lowMetalAlbedo * clamp(0.52 + albedoChroma * 0.46 + roughness * 0.12, 0.52, 0.92) ;
         float chromaPreserve = lowMetalAlbedo * smoothstep(0.035, 0.14, albedoChroma) * (1.0 - smoothstep(0.52, 0.80, roughness)) ;
         float smoothOpaquePaint = lowMetalAlbedo * (1.0 - smoothstep(0.52, 0.72, roughness)) ;
         vec3 albedoPreserve = mix(max(baseColor * 0.96, albedoLit), baseColor, max(chromaPreserve, smoothOpaquePaint)) ;
         Lo = mix(Lo, albedoPreserve, max(albedoWeight, smoothOpaquePaint * 0.88)) ;
      }
      vec3 emissiveFactor = unpackEmissive(pc.emissivePacked) ;
      vec3 emissive = emissiveFactor ;
      if(emissiveIndex < 1024u){
         vec4 eTex = texture(texSamplers[nonuniformEXT(emissiveIndex)], emissiveUV) ;
         vec3 eTexRgb = eTex.rgb ;
         if(extSpecularColorMap || extDiffuseTransmissionColor || extTransmissionMap || extDiffuseTransmissionMap){
            emissive = vec3(0.0) ;
         } else if(dot(emissiveFactor, emissiveFactor) <= 0.000001){
            emissive = eTexRgb ;
         } else {
            emissive = eTexRgb * emissiveFactor ;
         }
      }
      if(dbgBaseColor && dbgEmissive){
         vec3 emitDbg = emissive ;
         if(emissiveIndex < 1024u){
            emitDbg = texture(texSamplers[nonuniformEXT(emissiveIndex)], emissiveUV).rgb ;
         }
         outColor = vec4(linearToSrgb(clamp(emitDbg, 0.0, 1.0)), 1.0) ;
         return ;
      }
      if(dbgBaseColor){
         outColor = vec4(linearToSrgb(clamp(baseColor, 0.0, 1.0)), 1.0) ;
         return ;
      }
      if(dbgEmissive){
         outColor = vec4(linearToSrgb(clamp(emissive, 0.0, 1.0)), 1.0) ;
         return ;
      }
      float emissiveBoost = emissiveOnlySurface ? 1.85 : 1.12 ;
      vec3 litBase = max(Lo, vec3(0.0)) ;
      vec3 emitBase = max(emissive, vec3(0.0)) * emissiveBoost ;
      outLin = emissiveOnlySurface ? (emitBase + litBase * 0.02) : (litBase + emitBase) ;
   }
   vec3 safeOut = max(outLin, vec3(0.0)) ;
   vec3 mapped = neutralToneMap(safeOut) ;
   outColor = vec4(linearToSrgb(mapped), baseAlpha) ;
}
