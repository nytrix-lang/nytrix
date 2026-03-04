#version 450
layout(location=0) in vec4 vColor;
layout(location=1) in vec2 vUV;
layout(location=2) in vec3 vNormal;
layout(push_constant) uniform PC { mat4 vp; mat4 model; int isMask; int isUnlit; } pc;
layout(set=0, binding=0) uniform sampler2D texSampler;
layout(location=0) out vec4 outColor;
void main(){
  vec4 tex = texture(texSampler, vUV);
  if(pc.isMask != 0){ tex = vec4(1.0, 1.0, 1.0, tex.r); }
  if(pc.isUnlit != 0){
     vec4 base = vColor * tex;
     outColor = vec4(base.rgb * base.a, base.a);
  } else {
     vec3 normal = vNormal;
     float nl = length(normal);
     if(nl < 1e-5){ normal = vec3(0.0, 0.0, 1.0); }
     else { normal = normal / nl; }
     vec3 l = normalize(vec3(0.5, 1.0, 0.5));
     float diff = max(dot(normal, l), 0.1);
     vec3 skyCol = vec3(0.5, 0.7, 1.0); vec3 groundCol = vec3(0.12, 0.12, 0.15);
     vec3 ambient = mix(groundCol, skyCol, normal.y * 0.5 + 0.5) * 0.4;
     vec4 lit = vColor * tex * vec4(ambient + diff * 0.7, 1.0);
     outColor = vec4(lit.rgb * lit.a, lit.a);
  }
}
