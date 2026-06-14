;; Keywords: render vulkan gpu compute os ui
;; Vulkan compatibility facade for backend-neutral compute sidecar descriptors.
module std.os.ui.render.vk.compute(compute_caps, compute_workgroups, gltf_material_compute_shader, speculative_gi_probe_shader, ibl_prefilter_shader, brdf_lut_shader, transmission_blur_shader, material_ext_resolve_shader, refraction_resolve_shader, compute_pass_desc, compute_feature_mask, COMPUTE_FEATURE_REFRACTION, COMPUTE_FEATURE_TRANSMISSION, COMPUTE_FEATURE_VOLUME, COMPUTE_FEATURE_DIFFUSE_TRANSMISSION, COMPUTE_FEATURE_SPECULAR_GI, COMPUTE_FEATURE_MESH_INSTANCING)
use std.os.ui.render.compute as render_compute

def COMPUTE_FEATURE_REFRACTION = render_compute.COMPUTE_FEATURE_REFRACTION
def COMPUTE_FEATURE_TRANSMISSION = render_compute.COMPUTE_FEATURE_TRANSMISSION
def COMPUTE_FEATURE_VOLUME = render_compute.COMPUTE_FEATURE_VOLUME
def COMPUTE_FEATURE_DIFFUSE_TRANSMISSION = render_compute.COMPUTE_FEATURE_DIFFUSE_TRANSMISSION
def COMPUTE_FEATURE_SPECULAR_GI = render_compute.COMPUTE_FEATURE_SPECULAR_GI
def COMPUTE_FEATURE_MESH_INSTANCING = render_compute.COMPUTE_FEATURE_MESH_INSTANCING

fn compute_caps() dict { render_compute.compute_caps() }

fn compute_workgroups(int count, int local_size=64) int { render_compute.compute_workgroups(count, local_size) }

fn compute_feature_mask(any mat_info) int { render_compute.compute_feature_mask(mat_info) }

fn compute_pass_desc(any name, str shader, any local_x=8, any local_y=8, any local_z=1) dict { render_compute.compute_pass_desc(name, shader, local_x, local_y, local_z) }

fn gltf_material_compute_shader() str { render_compute.gltf_material_compute_shader() }

fn speculative_gi_probe_shader() str { render_compute.speculative_gi_probe_shader() }

fn ibl_prefilter_shader() str { render_compute.ibl_prefilter_shader() }

fn brdf_lut_shader() str { render_compute.brdf_lut_shader() }

fn transmission_blur_shader() str { render_compute.transmission_blur_shader() }

fn material_ext_resolve_shader() str { render_compute.material_ext_resolve_shader() }

fn refraction_resolve_shader() str { render_compute.refraction_resolve_shader() }
