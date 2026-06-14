;; Keywords: render gpu compute os ui
;; Backend-neutral compute shader sidecars for glTF materials, IBL, refraction, and GI probes.
;; Provides stable shader strings and descriptors for renderer integration.
module std.os.ui.render.compute(compute_caps, compute_workgroups, gltf_material_compute_shader, speculative_gi_probe_shader, ibl_prefilter_shader, brdf_lut_shader, transmission_blur_shader, material_ext_resolve_shader, refraction_resolve_shader, compute_pass_desc, compute_feature_mask, COMPUTE_FEATURE_REFRACTION, COMPUTE_FEATURE_TRANSMISSION, COMPUTE_FEATURE_VOLUME, COMPUTE_FEATURE_DIFFUSE_TRANSMISSION, COMPUTE_FEATURE_SPECULAR_GI, COMPUTE_FEATURE_MESH_INSTANCING)
use std.core

def COMPUTE_FEATURE_REFRACTION = 1
def COMPUTE_FEATURE_TRANSMISSION = 2
def COMPUTE_FEATURE_VOLUME = 4
def COMPUTE_FEATURE_DIFFUSE_TRANSMISSION = 8
def COMPUTE_FEATURE_SPECULAR_GI = 16
def COMPUTE_FEATURE_MESH_INSTANCING = 32

fn compute_caps() dict {
   "Reports renderer-neutral compute features planned by the material sidecar path."
   return {
      "refraction_resolve": true,
      "transmission_blur": true,
      "volume_attenuation": true,
      "diffuse_transmission": true,
      "speculative_gi_probes": true,
      "indirect_draw_prepare": true
   }
}

fn compute_workgroups(int count, int local_size=64) int {
   "Ceil-divide element count by local workgroup size for dispatch sizing."
   if count <= 0 { return 0 }
   def ls = local_size <= 0 ? 1 : local_size
   (count + ls - 1) / ls
}

fn compute_feature_mask(any mat_info) int {
   "Builds a bitmask of material extension features that require sidecar compute work."
   if !is_dict(mat_info) { return 0 }
   mut m = 0
   if float(mat_info.get("refraction_factor", 0.0)) > 0.0 { m = bor(m, COMPUTE_FEATURE_REFRACTION) }
   if float(mat_info.get("transmission_factor", 0.0)) > 0.0 { m = bor(m, COMPUTE_FEATURE_TRANSMISSION) }
   if float(mat_info.get("thickness_factor", 0.0)) > 0.0 { m = bor(m, COMPUTE_FEATURE_VOLUME) }
   if float(mat_info.get("diffuse_transmission_factor", 0.0)) > 0.0 { m = bor(m, COMPUTE_FEATURE_DIFFUSE_TRANSMISSION) }
   m
}

fn compute_pass_desc(any name, str shader, any local_x=8, any local_y=8, any local_z=1) dict {
   "Returns a backend-neutral compute pass descriptor."
   return {
      "name": to_str(name),
      "stage": "comp",
      "shader": shader,
      "local_x": int(local_x),
      "local_y": int(local_y),
      "local_z": int(local_z)
   }
}

fn gltf_material_compute_shader() str {
   "Future material resolve compute path. Inputs are deliberately abstract binding names for renderer-side wiring."
   "
   #version 450
   layout(local_size_x=8, local_size_y=8, local_size_z=1) in ;
   layout(binding=0, rgba16f) uniform readonly image2D colorIn ;
   layout(binding=1, rgba16f) uniform readonly image2D normalRoughnessIn ;
   layout(binding=2, rgba16f) uniform readonly image2D materialExtIn ;
   layout(binding=3, rgba16f) uniform writeonly image2D colorOut ;
   layout(push_constant) uniform PC { vec4 camera ; uint featureMask; float time; } pc;
   vec3 beerLambert(vec3 transmittanceColor, float distance, float thickness){
   float d = max(distance * max(thickness, 0.0), 0.0001) ;
   return exp(-max(vec3(0.0), vec3(1.0) - transmittanceColor) * d) ;
   }
   void main(){
   ivec2 p = ivec2(gl_GlobalInvocationID.xy) ;
   vec4 c = imageLoad(colorIn, p) ;
   vec4 nr = imageLoad(normalRoughnessIn, p) ;
   vec4 ex = imageLoad(materialExtIn, p) ;
   float transmission = ex.x ;
   float thickness = ex.y ;
   float ior = max(ex.z, 1.0) ;
   vec3 atten = beerLambert(max(c.rgb, vec3(0.001)), 1.0, thickness) ;
   vec3 resolved = mix(c.rgb, c.rgb * atten, clamp(transmission, 0.0, 1.0)) ;
   imageStore(colorOut, p, vec4(resolved, c.a)) ;
   }
   "
}

fn refraction_resolve_shader() str {
   "Screen-space refraction/rough transmission resolve. Wire sceneColor/depth/normal plus material extension G-buffer."
   "
   #version 450
   layout(local_size_x=8, local_size_y=8, local_size_z=1) in ;
   layout(binding=0) uniform sampler2D sceneColor ;
   layout(binding=1) uniform sampler2D sceneDepth ;
   layout(binding=2) uniform sampler2D normalRoughness ;
   layout(binding=3) uniform sampler2D materialExt ;
   layout(binding=4, rgba16f) uniform writeonly image2D outColor ;
   layout(push_constant) uniform PC { vec2 invExtent ; float maxOffset; float time; } pc;
   void main(){
   ivec2 ip = ivec2(gl_GlobalInvocationID.xy) ;
   vec2 uv = (vec2(ip) + vec2(0.5)) * pc.invExtent ;
   vec4 nr = texture(normalRoughness, uv) ;
   vec4 ex = texture(materialExt, uv) ;
   vec2 nxy = nr.xy * 2.0 - 1.0 ;
   float rough = clamp(nr.w, 0.0, 1.0) ;
   float refraction = clamp(ex.x, 0.0, 1.0) ;
   vec2 duv = nxy * pc.maxOffset * refraction * (1.0 - 0.5 * rough) * pc.invExtent ;
   vec3 col = texture(sceneColor, uv + duv).rgb ;
   imageStore(outColor, ip, vec4(col, 1.0)) ;
   }
   "
}

fn speculative_gi_probe_shader() str {
   "Very small probe irradiance accumulator scaffold for future Sponza-scale GI."
   "
   #version 450
   layout(local_size_x=64, local_size_y=1, local_size_z=1) in ;
   struct Probe { vec4 posRadius ; vec4 irradiance; };
   layout(std430, binding=0) buffer Probes { Probe probes[] ; };
   layout(binding=1) uniform samplerCube envMap ;
   layout(push_constant) uniform PC { uint probeCount ; float intensity; float time; uint flags; } pc;
   void main(){
   uint i = gl_GlobalInvocationID.x ;
   if(i >= pc.probeCount) return ;
   vec3 n = normalize(vec3(fract(float(i)*0.37)-0.5, fract(float(i)*0.73)-0.5, 0.5)) ;
   vec3 e = texture(envMap, n).rgb ;
   probes[i].irradiance = vec4(mix(probes[i].irradiance.rgb, e * pc.intensity, 0.05), 1.0) ;
   }
   "
}

fn ibl_prefilter_shader() str {
   "Unwired compute shader scaffold for specular IBL prefiltering."
   "
   #version 450
   layout(local_size_x=8, local_size_y=8, local_size_z=1) in ;
   layout(binding=0) uniform samplerCube envMap ;
   layout(binding=1, rgba16f) uniform writeonly imageCube outMip ;
   layout(push_constant) uniform PC { float roughness ; uint face; uint size; uint sampleCount; } pc;
   void main(){
   ivec2 p = ivec2(gl_GlobalInvocationID.xy) ;
   if(p.x >= int(pc.size) || p.y >= int(pc.size)) return ;
   vec2 uv = (vec2(p)+0.5)/float(pc.size)*2.0-1.0 ;
   vec3 dir = normalize(vec3(uv, 1.0)) ;
   vec3 c = texture(envMap, dir).rgb ;
   imageStore(outMip, ivec3(p, int(pc.face)), vec4(c, 1.0)) ;
   }
   "
}

fn brdf_lut_shader() str {
   "Unwired split-sum BRDF LUT shader scaffold."
   "
   #version 450
   layout(local_size_x=8, local_size_y=8, local_size_z=1) in ;
   layout(binding=0, rg16f) uniform writeonly image2D outLut ;
   layout(push_constant) uniform PC { uint size ; uint sampleCount; } pc;
   void main(){
   ivec2 p = ivec2(gl_GlobalInvocationID.xy) ;
   if(p.x >= int(pc.size) || p.y >= int(pc.size)) return ;
   vec2 uv = (vec2(p)+0.5)/float(pc.size) ;
   imageStore(outLut, p, vec4(uv.x, uv.y, 0.0, 1.0)) ;
   }
   "
}

fn transmission_blur_shader() str {
   "Unwired rough-transmission blur pass. Use after opaque color resolve and before transparent composite."
   "
   #version 450
   layout(local_size_x=8, local_size_y=8, local_size_z=1) in ;
   layout(binding=0) uniform sampler2D sceneColor ;
   layout(binding=1) uniform sampler2D materialExt ;
   layout(binding=2, rgba16f) uniform writeonly image2D outColor ;
   layout(push_constant) uniform PC { vec2 invExtent ; float maxRadius; float lodBias; } pc;
   void main(){
   ivec2 ip = ivec2(gl_GlobalInvocationID.xy) ;
   vec2 uv = (vec2(ip)+0.5)*pc.invExtent ;
   vec4 ex = texture(materialExt, uv) ;
   float r = clamp(ex.y * pc.maxRadius, 0.0, pc.maxRadius) ;
   vec3 c = vec3(0.0) ; float w = 0.0;
   for(int y=-2 ;y<=2;y++) for(int x=-2;x<=2;x++){ vec2 o=vec2(x,y)*pc.invExtent*r; c += texture(sceneColor, uv+o).rgb; w += 1.0; }
   imageStore(outColor, ip, vec4(c/max(w,1.0), 1.0)) ;
   }
   "
}

fn material_ext_resolve_shader() str {
   "Unwired material-extension G-buffer writer. Intended to be generated into fragment or compute path later."
   "
   #version 450
   layout(local_size_x=64) in ;
   struct MaterialExt { uint featureMask ; uint bsdf0; uint bsdf1; uint bsdf2; uint bsdf3; uint bsdf4; uint bsdf5; uint pad; };
   layout(std430,binding=0) readonly buffer Materials { MaterialExt mats[] ; };
   layout(std430,binding=1) buffer Out { vec4 ext[] ; };
   layout(push_constant) uniform PC { uint materialCount ; } pc;
   void main(){ uint i=gl_GlobalInvocationID.x ; if(i>=pc.materialCount) return; MaterialExt m=mats[i]; ext[i]=vec4(float(m.bsdf4&255u)/255.0,float((m.bsdf5>>8)&255u)/255.0,0.0,1.0); }
   "
}
