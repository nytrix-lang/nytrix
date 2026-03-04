#version 450
layout(location=0) in vec3 inPos;
layout(location=1) in vec2 inUV;
layout(location=2) in vec4 inColor;
layout(location=3) in uint inTexIndex;
layout(location=4) in vec3 inNormal;
layout(set=1, binding=0, std140) uniform UBO { mat4 vp; mat4 model; ivec4 flags; } ubo;
layout(location=0) out vec4 vColor;
layout(location=1) out vec2 vUV;
layout(location=2) out vec3 vNormal;
layout(location=3) flat out uint vTexIndex;
void main(){
  gl_Position = ubo.vp * ubo.model * vec4(inPos, 1.0);
  vColor = inColor;
  vUV = inUV;
  vNormal = mat3(ubo.model) * inNormal;
  vTexIndex = inTexIndex;
}
