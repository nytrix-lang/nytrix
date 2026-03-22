#version 450
layout(location=0) in vec3 inPos ;
layout(location=1) in vec2 inUV ;
layout(location=2) in vec4 inColor ;
layout(location=3) in uint inTexIndex ;
layout(location=4) in vec3 inNormal ;
layout(push_constant) uniform PC { mat4 vp ; mat4 model; int isMask; int isUnlit; uint baseColor; uint material; int viewportW; int viewportH; } pc;
layout(location=0) out vec4 vColor ;
layout(location=1) out vec2 vUV ;
layout(location=2) out vec3 vNormal ;
layout(location=3) flat out uint vTexIndex ;
void main(){
  gl_Position = pc.vp * pc.model * vec4(inPos, 1.0) ;
  vColor = inColor ;
  vUV = inUV ;
  vNormal = inNormal ;
  vTexIndex = inTexIndex ;
}