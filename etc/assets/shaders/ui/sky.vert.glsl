#version 450
layout(location=0) in vec3 inPos ;
layout(location=1) in vec2 inUV ;
layout(location=2) in vec4 inColor ;
layout(location=3) in uint inTexIndex ;
layout(location=4) in vec3 inNormal ;
layout(push_constant) uniform PC {
   mat4 vp ;
   mat4 model ;
   int isMask ;
   int isUnlit ;
   uint baseColor ;
   uint material ;
   int viewportW ;
   int viewportH ;
   layout(offset = 152) float time ;
   layout(offset = 156) float rainbow ;
   layout(offset = 160) vec3 camPos ;
} pc ;
layout(location=0) out vec3 vDir ;
layout(location=1) flat out uint vTexIndex ;
void main(){
   vDir = inPos ;
   vTexIndex = inTexIndex ;
   gl_Position = (pc.vp * vec4(inPos, 1.0)).xyww ;
}
