;; Focused mesh API sharing ownership and renderer state with the legacy facade.
module core.os.ui.render.mesh(
   mesh_load, mesh_create, mesh_create_cpu, mesh_create_static,
   mesh_create_ex, mesh_create_indexed, mesh_create_cpu_part_from_raw,
   mesh_set_bounds, mesh_fit_world, mesh_fit_perspective, mesh_fit_camera,
   mesh_destroy, mesh_group_destroy, mesh_retire, mesh_collect_retired,
   mesh_build_grid, mesh_build_axes, draw_mesh, draw_mesh_group, set_wireframe
)

use core.os.ui.render (
   mesh_load, mesh_create, mesh_create_cpu, mesh_create_static,
   mesh_create_ex, mesh_create_indexed, mesh_create_cpu_part_from_raw,
   mesh_set_bounds, mesh_fit_world, mesh_fit_perspective, mesh_fit_camera,
   mesh_destroy, mesh_group_destroy, mesh_retire, mesh_collect_retired,
   mesh_build_grid, mesh_build_axes, draw_mesh, draw_mesh_group, set_wireframe
)
