#version 450
layout(location=0) in vec4 vColor;
layout(location=1) in vec2 vUV;
layout(location=2) in vec3 vNormal;
layout(location=3) flat in uint vTexIndex;
layout(location=0) out vec4 outColor;

void main(){
  float halfLen = max(vNormal.x, 0.0);
  float radius = max(vNormal.y, 0.5);
  vec2 nearest = vec2(clamp(vUV.x, -halfLen, halfLen), 0.0);
  float d = length(vUV - nearest) - radius;
  float fw = max(fwidth(d), 0.001);
  float alpha = clamp(0.5 - d / fw, 0.0, 1.0);
  if(alpha <= 0.0) discard;
  outColor = vec4(vColor.rgb, vColor.a * alpha);
}
