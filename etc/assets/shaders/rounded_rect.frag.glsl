#version 450
#extension GL_EXT_nonuniform_qualifier : enable
layout(set=0, binding=0) uniform sampler2D texSamplers[1024];
layout(location=0) in vec4 vColor;
layout(location=1) in vec2 vUV;
layout(location=2) in vec3 vNormal;
layout(location=3) flat in uint vTexIndex;
layout(location=4) in vec4 vClip;
layout(push_constant) uniform PC {
  mat4 vp;
  mat4 model;
  int isMask;
  int isUnlit;
  uint baseColor;
  uint material;
  int viewportW;
  int viewportH;
} pc;
layout(location=0) out vec4 outColor;

vec4 captureAt(uint slot, vec2 uv) {
  return texture(texSamplers[nonuniformEXT(slot)], clamp(uv, vec2(0.0), vec2(1.0)));
}

void main() {
  vec2 size = max(vNormal.xy, vec2(1.0));
  float radius = clamp(vNormal.z, 0.0, min(size.x, size.y) * 0.5);
  bool glass = (vTexIndex & 0x40000000u) != 0u;
  bool shadow = !glass && (vTexIndex & 1u) != 0u;
  bool outline = !glass && (vTexIndex & 2u) != 0u;
  vec2 local = shadow ? vUV : (vUV * size);
  if (!glass && vClip.z > 0.0 && (local.x < vClip.x || local.x > vClip.x + vClip.z ||
                                  local.y < vClip.y || local.y > vClip.y + vClip.w)) discard;
  vec2 p = shadow ? (vUV - size * 0.5) : ((vUV - vec2(0.5)) * size);
  vec2 q = abs(p) - (size * 0.5 - vec2(radius));
  float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - radius;
  float fw = max(fwidth(d), 0.001);
  float alpha;
  if (glass) {
    alpha = clamp(0.5 - d / fw, 0.0, 1.0);
    if (alpha <= 0.0) discard;
    uint slot = vTexIndex & 0x3ffu;
    vec2 texel = 1.0 / vec2(textureSize(texSamplers[nonuniformEXT(slot)], 0));
    vec2 screenUv = gl_FragCoord.xy * texel;
    float blurPx = clamp(vClip.x, 0.0, 24.0);
    vec2 blurStep = texel * blurPx * 0.55;
    vec2 normal = normalize(vec2(dFdx(d), dFdy(d)) + vec2(0.00001));
    float edge = 1.0 - smoothstep(0.0, max(radius, 1.0), max(d, 0.0));
    vec2 refractUv = screenUv + normal * texel * clamp(vClip.y, 0.0, 8.0) * edge;
    vec3 blurred = captureAt(slot, refractUv).rgb * 0.24;
    blurred += captureAt(slot, refractUv + vec2(blurStep.x, 0.0)).rgb * 0.14;
    blurred += captureAt(slot, refractUv - vec2(blurStep.x, 0.0)).rgb * 0.14;
    blurred += captureAt(slot, refractUv + vec2(0.0, blurStep.y)).rgb * 0.14;
    blurred += captureAt(slot, refractUv - vec2(0.0, blurStep.y)).rgb * 0.14;
    blurred += captureAt(slot, refractUv + blurStep).rgb * 0.10;
    blurred += captureAt(slot, refractUv - blurStep).rgb * 0.10;
    float rim = (1.0 - smoothstep(0.0, max(vClip.z, 0.25), abs(d))) * clamp(vClip.w, 0.0, 1.0);
    vec3 tinted = mix(blurred, vColor.rgb, 0.20);
    outColor = vec4(tinted + vec3(rim), vColor.a * alpha);
    return;
  }
  if (shadow) {
    float blur = max(float(vTexIndex >> 1u) / 16.0, 0.5);
    alpha = 1.0 - smoothstep(-fw, blur + fw, d);
  } else if (outline) {
    float stroke = max(float(vTexIndex >> 2u) / 16.0, 0.5);
    float ring = abs(d + stroke * 0.5) - stroke * 0.5;
    alpha = clamp(0.5 - ring / fw, 0.0, 1.0);
  } else {
    alpha = clamp(0.5 - d / fw, 0.0, 1.0);
  }
  if (alpha <= 0.0) discard;
  outColor = vec4(vColor.rgb, vColor.a * alpha);
}
