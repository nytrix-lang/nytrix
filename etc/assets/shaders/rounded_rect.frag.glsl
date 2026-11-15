#version 450
layout(location=0) in vec4 vColor;
layout(location=1) in vec2 vUV;
layout(location=2) in vec3 vNormal;
layout(location=3) flat in uint vTexIndex;
layout(location=0) out vec4 outColor;

void main(){
  vec2 size = max(vNormal.xy, vec2(1.0));
  float radius = clamp(vNormal.z, 0.0, min(size.x, size.y) * 0.5);
  vec2 p = (vUV - vec2(0.5)) * size;
  vec2 q = abs(p) - (size * 0.5 - vec2(radius));
  float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - radius;
  float fw = max(fwidth(d), 0.001);
  float alpha = clamp(0.5 - d / fw, 0.0, 1.0);
  if(alpha <= 0.0) discard;
  outColor = vec4(vColor.rgb, vColor.a * alpha);
}
