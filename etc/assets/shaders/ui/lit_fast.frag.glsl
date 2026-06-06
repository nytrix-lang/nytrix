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
vec4 unpackColor(uint p){ return vec4(float(p & 255u), float((p >> 8u) & 255u), float((p >> 16u) & 255u), float((p >> 24u) & 255u)) / 255.0 ; }
vec3 unpackEmissive(uint p){
  float s = float((p >> 24u) & 255u) * (64.0 / 255.0) ;
  if(s <= 0.0){ return vec3(0.0) ; }
  return vec3(float(p & 255u), float((p >> 8u) & 255u), float((p >> 16u) & 255u)) * (s / 255.0) ;
}

float sat1(float x){ return clamp(x, 0.0, 1.0) ; }
vec3 sat3(vec3 x){ return clamp(x, vec3(0.0), vec3(1.0)) ; }
float max3(vec3 v){ return max(v.r, max(v.g, v.b)) ; }
float pow5(float x){ x = sat1(x) ; float x2 = x * x ; return x2 * x2 * x ; }

float decodeUvOffset16(uint q){ return mix(-8.0, 8.0, float(q) / 65535.0) ; }
float decodeUvScale11(uint q){
  uint v = q & 2047u ;
  return v == 0u ? 1.0 : (float(v) / 2047.0) * 64.0 - 32.0 ;
}
float decodeUvRot8(uint q){ return (float(q & 255u) / 255.0) * (2.0 * PI) - PI ; }
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
  return vNormal / nl ;
}
float decodeNormalScale7(uint packedIndex){
  float s = float((packedIndex >> 24u) & 127u) / 127.0 * 2.0 ;
  return s <= 0.0001 ? 1.0 : s ;
}
vec3 applyNormalMap(vec3 N, vec4 authoredTangent, uint packedIndex, uint xf0, uint xf1){
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
  vec3 mapN = texture(texSamplers[nonuniformEXT(normalIndex)], uv).xyz * 2.0 - 1.0 ;
  mapN.xy *= decodeNormalScale7(packedIndex) ;
  return normalize(mat3(T, B, N) * mapN) ;
}

float D_GGX(float NoH, float r){
  float a = max(r * r, 0.045) ;
  float a2 = a * a ;
  float d = NoH * NoH * (a2 - 1.0) + 1.0 ;
  return a2 / max(PI * d * d, 1e-4) ;
}
float V_SmithFast(float NoV, float NoL, float r){
  float k = (r + 1.0) ;
  k = (k * k) * 0.125 ;
  float gv = NoV / max(NoV * (1.0 - k) + k, 1e-4) ;
  float gl = NoL / max(NoL * (1.0 - k) + k, 1e-4) ;
  return gv * gl ;
}
vec3 F_Schlick(vec3 f0, float VoH){
  return f0 + (vec3(1.0) - f0) * pow5(1.0 - VoH) ;
}

void main(){
  bool vertexColorPrimary = (pc.baseTexIndex & 0x40000000u) != 0u ;
  bool vertexColorMultiply = (pc.baseTexIndex & 0x10000000u) != 0u ;
  bool vertexTextureIndex = (pc.baseTexIndex & 0x04000000u) != 0u ;
  bool vertexPackedMaterial = (pc.baseTexIndex & 0x08000000u) != 0u ;
  vec4 baseTint = ((vertexColorPrimary || vertexColorMultiply) ? vColor : vec4(1.0)) * unpackColor(pc.baseColor) ;

  uint baseIndex = pc.baseTexIndex ;
  if((baseIndex & 0x80000000u) != 0u){
    uint vertexIndex = vTexIndex & 0xFFFFu ;
    // UI/font batches can legitimately use texture slot 0. Sampling the default
    // white texture is also the correct fallback for untextured vertex-color UI.
    baseIndex = (vertexTextureIndex && vertexIndex < 1024u) ? vertexIndex : 0xFFFFFFFFu ;
  } else {
    baseIndex = baseIndex & 0xFFFFu ;
  }

  vec4 tex = vec4(1.0) ;
  vec2 buv = baseUv() ;
  if(!vertexColorPrimary && baseIndex < 1024u){ tex = texture(texSamplers[nonuniformEXT(baseIndex)], buv) ; }

  uint alphaMode = pc.alphaPacked & 3u ;
  float alphaCutoff = float((pc.alphaPacked >> 8u) & 255u) / 255.0 ;
  float rawAlpha = tex.a * baseTint.a ;
  bool vertexTextureCoverage = (pc.baseTexIndex & 0x80000000u) != 0u && (vertexColorMultiply || vertexTextureIndex) && baseIndex < 1024u ;
  bool vertexCoverageMask = (alphaMode == 2u || vertexTextureCoverage) && vertexColorMultiply && baseIndex < 1024u ;
  if(vertexCoverageMask){
    float rgbCoverage = max(max(tex.r, tex.g), tex.b) ;
    float coverage = max(tex.a, rgbCoverage) ;
    rawAlpha = coverage * baseTint.a ;
  }
  float outAlpha = (alphaMode == 2u || vertexTextureCoverage) ? rawAlpha : 1.0 ;
  if(alphaMode == 1u){
    if(rawAlpha < alphaCutoff){ discard ; }
    outAlpha = 1.0 ;
	  } else if((alphaMode == 2u || vertexTextureCoverage) && outAlpha <= 0.001){
	    discard ;
	  }
	  bool gltfCullMaterial = (pc.normalTexIndex & 0x80000u) != 0u ;
	  bool doubleSidedMaterial = (pc.normalTexIndex & 0x40000u) != 0u ;
	  bool mirroredFacing = (pc.normalTexIndex & 0x20000u) != 0u ;
	  bool logicalBackface = mirroredFacing ? false : !gl_FrontFacing ;
	  if(gltfCullMaterial && !doubleSidedMaterial && !gl_FrontFacing){ discard ; }

	  vec3 baseColor = vertexColorPrimary ? baseTint.rgb : (vertexCoverageMask ? baseTint.rgb : tex.rgb * baseTint.rgb) ;
	  if(pc.isUnlit != 0){
	    outColor = vec4(linearToSrgb(sat3(baseColor)), outAlpha) ;
    return ;
  }

	  vec3 N = getNormal() ;
	  if(logicalBackface){ N = -N ; }
  N = applyNormalMap(N, vTangent, pc.normalTexIndex, pc.normalUvXf0, pc.normalUvXf1) ;

  vec3 V = normalize(pc.camPos - vWorldPos + vec3(0.00001)) ;
  float NoV = max(dot(N, V), 0.001) ;

  float metallic = vertexPackedMaterial ? vColor.r : float(pc.material & 255u) / 255.0 ;
  float roughness = max(vertexPackedMaterial ? vColor.g : float((pc.material >> 8u) & 255u) / 255.0, 0.04) ;
  uint mrPacked = vertexPackedMaterial ? 0u : (pc.material >> 16u) ;
  uint mrWord = mrPacked & 0x7FFFu ;
  bool hasMrTexture = mrWord != 0u ;
  uint mrIndex = hasMrTexture ? (mrWord - 1u) : 0xFFFFFFFFu ;
  if(hasMrTexture && mrIndex < 1024u){
    vec2 mrUV = applyUvXf(((mrPacked & 0x8000u) != 0u) ? vUV2 : vUV, pc.mrUvXf0, pc.mrUvXf1) ;
    vec4 mrTex = texture(texSamplers[nonuniformEXT(mrIndex)], mrUV) ;
    roughness = max(roughness * mrTex.g, 0.04) ;
    float mt = mrTex.b ;
    mt = mt < 0.04 ? 0.0 : (mt > 0.96 ? 1.0 : mt) ;
    metallic = clamp(metallic * mt, 0.0, 1.0) ;
  }

  float specularFactor = float(pc.bsdf0Packed & 255u) / 255.0 ;
  vec3 specularColor = vec3(float(pc.bsdf1Packed & 255u), float((pc.bsdf1Packed >> 8u) & 255u), float((pc.bsdf1Packed >> 16u) & 255u)) / 255.0 ;
  vec3 F0 = mix(vec3(0.04) * max(specularColor, vec3(0.001)) * specularFactor, baseColor, metallic) ;
  vec3 diffuseColor = baseColor * (1.0 - metallic) ;

  float anySceneLight = 0.0 ;
  vec3 Lo = vec3(0.0) ;
#ifndef NY_FAST_ENV_ONLY
  for(int li = 0 ; li < 8 ; li++){
    vec4 lPosType = scene.lightPosType[li] ;
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
        float nd = clamp(dist / lRange, 0.0, 1.0) ;
        float fall = clamp(1.0 - nd * nd * nd * nd, 0.0, 1.0) ;
        attenuation *= fall * fall ;
      }
      if(lType > 1.5){
        float outerCos = lDirOuter.w ;
        float innerCos = outerCos + 0.1 ;
        attenuation *= clamp((dot(-L, normalize(lDirOuter.xyz)) - outerCos) / max(innerCos - outerCos, 0.001), 0.0, 1.0) ;
      }
    }
    float NoL = max(max(dot(N, L), 0.0), max(dot(-N, L), 0.0) * 0.55) ;
    if(NoL <= 0.0){ continue ; }
    vec3 H = normalize(L + V) ;
    float NoH = max(dot(N, H), 0.0) ;
    float VoH = max(dot(V, H), 0.0) ;
    vec3 F = F_Schlick(F0, VoH) ;
    float spec = D_GGX(NoH, roughness) * V_SmithFast(NoV, NoL, roughness) ;
    Lo += (diffuseColor * (1.0 - max3(F)) / PI + F * spec) * lColor * attenuation * NoL ;
  }

  if(anySceneLight < 0.5){
#endif
    vec3 skyCol = vec3(0.78, 0.86, 0.98) ;
    vec3 groundCol = vec3(0.10, 0.09, 0.08) ;
    vec3 hemi = mix(groundCol, skyCol, clamp(N.y * 0.5 + 0.5, 0.0, 1.0)) ;
    vec3 L = normalize(vec3(-0.55, 0.82, 0.18)) ;
    vec3 H = normalize(L + V) ;
    float NoLFront = max(dot(N, L), 0.0) ;
    float NoLBack = max(dot(-N, L), 0.0) ;
    float NoLDiff = max(NoLFront, NoLBack * 0.55) ;
    float VoH = max(dot(V, H), 0.0) ;
    vec3 F = F_Schlick(F0, VoH) ;
    float spec = D_GGX(max(dot(N, H), 0.0), roughness) * V_SmithFast(NoV, max(NoLFront, 0.001), roughness) ;
    Lo += diffuseColor * hemi * 0.28 ;
    Lo += (diffuseColor * (1.0 - max3(F)) / PI) * vec3(0.92, 0.90, 0.86) * NoLDiff * 0.90 ;
    Lo += (F * spec) * vec3(0.92, 0.90, 0.86) * NoLFront * 0.90 ;
    vec3 R = reflect(-V, N) ;
    vec3 env = mix(groundCol, skyCol, clamp(R.y * 0.5 + 0.5, 0.0, 1.0)) ;
    Lo += env * F0 * mix(0.08, 0.42, metallic) * (1.0 - roughness * 0.60) ;
#ifndef NY_FAST_ENV_ONLY
  }
#endif

  float diffuseTransmission = float(pc.bsdf5Packed & 255u) / 255.0 ;
  float transmission = float((pc.bsdf0Packed >> 16u) & 255u) / 255.0 ;
  float iridescence = float((pc.bsdf0Packed >> 24u) & 255u) / 255.0 ;
  float ior = 1.0 + float((pc.bsdf1Packed >> 24u) & 255u) / 255.0 * 1.5 ;
  float thickness = (float((pc.bsdf2Packed >> 24u) & 255u) / 255.0) * 4.0 ;
  vec3 attenuationColor = vec3(float(pc.bsdf3Packed & 255u), float((pc.bsdf3Packed >> 8u) & 255u), float((pc.bsdf3Packed >> 16u) & 255u)) / 255.0 ;
  uint bsdf3A = (pc.bsdf3Packed >> 24u) & 255u ;
  if(iridescence > 0.001 && bsdf3A == 254u){ attenuationColor = vec3(1.0) ; }
  float dispersion = (float((pc.bsdf4Packed >> 24u) & 255u) / 255.0) * 10.0 ;
  bool diffuseTransmissionAlphaCard = alphaMode != 0u && diffuseTransmission > 0.001 ;
  uint occIndex = pc.occlusionTexIndex & 0xFFFFu ;
  if(occIndex != 0u && occIndex < 1024u){
    vec2 occUV = mapUv(pc.occlusionTexIndex, pc.occlusionUvXf0, pc.occlusionUvXf1) ;
    float occ = texture(texSamplers[nonuniformEXT(occIndex)], occUV).r ;
    float occStrength = float((pc.alphaPacked >> 16u) & 255u) / 255.0 ;
    if(transmission > 0.001 || diffuseTransmission > 0.001){
      occStrength = 0.0 ;
    }
    Lo *= mix(1.0, occ, occStrength) ;
  }

  if(transmission > 0.001){
    float fresnel = pow5(1.0 - NoV) ;
    float volumeGate = smoothstep(0.015, 0.55, thickness) ;
    float attLum = dot(attenuationColor, vec3(0.299, 0.587, 0.114)) ;
    float attChroma = max3(abs(attenuationColor - vec3(attLum))) ;
    float baseLum = dot(baseColor, vec3(0.299, 0.587, 0.114)) ;
    float baseChroma = max3(abs(baseColor - vec3(baseLum))) ;
    float amberCue = clamp(
      transmission * max(volumeGate, smoothstep(0.08, 0.46, baseChroma)) *
      smoothstep(0.28, 0.86, max(baseColor.r, attenuationColor.r)) *
      (1.0 - smoothstep(0.16, 0.58, max(baseColor.b, attenuationColor.b))) *
      (0.55 + 0.45 * max(attChroma, baseChroma)),
      0.0,
      1.0
    ) ;
    vec3 R = reflect(-V, N) ;
    vec3 fastSky = vec3(0.78, 0.86, 0.98) ;
    vec3 fastGround = vec3(0.10, 0.09, 0.08) ;
    vec3 fastEnv = mix(fastGround, fastSky, clamp(R.y * 0.5 + 0.5, 0.0, 1.0)) ;
    vec3 volumeTint = mix(sqrt(max(baseColor, vec3(0.0))), attenuationColor, clamp(volumeGate * (0.42 + attChroma), 0.0, 0.85)) ;
    vec3 clearGlass = fastEnv * volumeTint * (0.20 + 0.34 * NoV) ;
    clearGlass += fastEnv * F_Schlick(mix(vec3(0.04), vec3(0.12), clamp((ior - 1.0) / 1.5, 0.0, 1.0)), NoV) * (0.24 + 1.45 * fresnel) * (1.0 - roughness * 0.55) ;
    vec3 amberBody = mix(vec3(0.045, 0.012, 0.0015), vec3(0.38, 0.10, 0.016), 0.34 + 0.44 * NoV) ;
    amberBody *= mix(0.78, 1.16, clamp(baseLum, 0.0, 1.0)) ;
    vec3 amberGloss = fastEnv * vec3(1.25, 0.52, 0.14) * (0.030 + 1.35 * fresnel) * (1.0 - roughness * 0.50) ;
    vec3 amberGlass = amberBody + amberGloss ;
    if(dispersion > 0.05){
      vec3 fringe = vec3(0.56, 0.74, 1.18) * (0.10 + 0.42 * fresnel) * clamp(dispersion / 10.0, 0.0, 1.0) ;
      clearGlass += fringe ;
    }
    vec3 glassLo = mix(clearGlass, amberGlass, amberCue) ;
    float glassResolve = clamp(transmission * (0.46 + 0.32 * volumeGate + 0.16 * amberCue) * (1.0 - roughness * 0.20), 0.0, 0.90) ;
    Lo = mix(Lo, glassLo, glassResolve) ;
    outAlpha = 1.0 ;
  }

  if(iridescence > 0.001){
    float iriRim = pow5(1.0 - NoV) ;
    float band = 6.28318 * (0.16 + iriRim * 0.72 + roughness * 0.18) ;
    vec3 iriTint = 0.55 + 0.45 * cos(band + vec3(0.0, 2.1, 4.2)) ;
    vec3 iriEnv = mix(vec3(0.10, 0.09, 0.12), vec3(0.76, 0.86, 1.0), clamp(reflect(-V, N).y * 0.5 + 0.5, 0.0, 1.0)) ;
    Lo += iriEnv * iriTint * iridescence * (0.055 + 0.46 * iriRim) * (1.0 - metallic * 0.40) ;
  }

  if(anySceneLight < 0.5 && baseIndex < 1024u && metallic <= 0.05 && transmission <= 0.001 && diffuseTransmission <= 0.001 && iridescence <= 0.001){
    float albedoLum = dot(baseColor, vec3(0.299, 0.587, 0.114)) ;
    float albedoChroma = max3(abs(baseColor - vec3(albedoLum))) ;
    float albedoFloor = clamp(0.18 + roughness * 0.38 + albedoChroma * 0.22, 0.22, 0.62) ;
    float litLum = dot(max(Lo, vec3(0.0)), vec3(0.299, 0.587, 0.114)) ;
    vec3 albedoTarget = baseColor * max(albedoFloor, litLum * 1.10) ;
    vec3 albedoLit = max(albedoTarget, baseColor * max(albedoFloor, 0.42)) ;
    Lo = mix(Lo, albedoLit, clamp(0.54 + albedoChroma * 0.36, 0.54, 0.84)) ;
  }

  vec3 emissive = unpackEmissive(pc.emissivePacked) ;
  uint emissiveIndex = pc.emissiveTexIndex ;
  if((emissiveIndex & 0x80000000u) != 0u){ emissiveIndex = 0xFFFFFFFFu ; }
  else { emissiveIndex = emissiveIndex & 0xFFFFu ; }
  if(emissiveIndex < 1024u){
    vec2 eUV = mapUv(pc.emissiveTexIndex, pc.emissiveUvXf0, pc.emissiveUvXf1) ;
    vec3 eTex = texture(texSamplers[nonuniformEXT(emissiveIndex)], eUV).rgb ;
    emissive = dot(emissive, emissive) <= 0.000001 ? eTex : eTex * emissive ;
  }

  if(diffuseTransmissionAlphaCard){
    vec3 cardFill = mix(baseColor, sqrt(max(baseColor, vec3(0.0))), 0.94) ;
    float cardWrap = 0.38 + 0.62 * max(dot(-N, V), 0.0) ;
    Lo += cardFill * cardWrap * (0.95 + 1.20 * diffuseTransmission) * (1.0 - metallic) ;
    outAlpha = max(outAlpha, 0.94) ;
  }

  vec3 lit = max(Lo + emissive * 1.08, vec3(0.0)) ;
  outColor = vec4(linearToSrgb(sat3(lit)), outAlpha) ;
}
