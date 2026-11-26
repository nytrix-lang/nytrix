#version 450
layout(location=0) in vec3 inPos ;
layout(location=1) in vec2 inUV ;
layout(location=2) in vec4 inColor ;
layout(location=3) in uint inTexIndex ;
layout(location=4) in vec3 inNormal ;
layout(location=5) in vec4 inTangent ;
layout(location=6) in vec2 inUV2 ;
layout(push_constant) uniform PC { mat4 vp ; mat4 model; int isMask; int isUnlit; uint baseColor; uint material; int viewportW; int viewportH; uint bsdf4Packed; uint bsdf5Packed; layout(offset=160) vec3 camPos; layout(offset=172) uint emissivePacked; layout(offset=176) uint emissiveTexIndex; layout(offset=180) uint baseTexIndex; layout(offset=184) uint alphaPacked; layout(offset=188) uint occlusionTexIndex; layout(offset=192) uint bsdf0Packed; layout(offset=196) uint bsdf1Packed; layout(offset=200) uint bsdf2Packed; layout(offset=204) uint bsdf3Packed; layout(offset=208) uint baseUvXf0; layout(offset=212) uint baseUvXf1; layout(offset=216) uint normalUvXf0; layout(offset=220) uint normalUvXf1; layout(offset=224) uint mrUvXf0; layout(offset=228) uint mrUvXf1; layout(offset=232) uint occlusionUvXf0; layout(offset=236) uint occlusionUvXf1; layout(offset=240) uint emissiveUvXf0; layout(offset=244) uint emissiveUvXf1; layout(offset=248) uint normalTexIndex; layout(offset=252) uint mrTexIndex; } pc;
layout(location=0) out vec4 vColor ;
layout(location=1) out vec2 vUV ;
layout(location=2) out vec3 vNormal ;
layout(location=3) flat out uint vTexIndex ;
layout(location=4) out vec3 vWorldPos ;
layout(location=5) out vec4 vTangent ;
layout(location=6) out vec2 vUV2 ;
void main(){
  vec4 wp = pc.model * vec4(inPos, 1.0) ;
  vWorldPos = wp.xyz ;
  gl_Position = pc.vp * wp ;
  gl_PointSize = 1.0 ;
  bool modelIdentityHint = (pc.isMask & 2) != 0 ;
  mat3 model3 = mat3(pc.model) ;
  bool negDet = !modelIdentityHint && determinant(model3) < 0.0 ;
  mat3 normalMat = modelIdentityHint ? mat3(1.0) : transpose(inverse(model3)) ;
  vNormal = normalMat * inNormal ;
  vTangent = vec4(modelIdentityHint ? inTangent.xyz : (model3 * inTangent.xyz), negDet ? -inTangent.w : inTangent.w) ;
  vColor = inColor ;
  vUV = inUV ;
  vTexIndex = inTexIndex ;
  vUV2 = inUV2 ;
}
