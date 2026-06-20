#version 450
layout(location=0) in vec4 vColor ; layout(location=1) in vec2 vUV; layout(location=2) in vec3 vNormal; layout(location=3) flat in uint vTexIndex;
layout(location=0) out vec4 outColor ;
void main(){
  vec2 uv = vUV * 2.0 - 1.0 ;
  float d = length(uv) ;
  float alpha = clamp(0.5 - (d - 1.0) / max(fwidth(d), 0.001), 0.0, 1.0) ;
  if(alpha <= 0.0) discard ;
  float outA = vColor.a * alpha ;
  outColor = vec4(vColor.rgb, outA) ;
}
