#!/bin/ny

use std.core *
use std.os *
use std.os.thread *
use std.math *
use std.ui.consts *
use std.ui.gfx *
use std.ui.window as window
use std.ui.input as uin
use std.ui.camera as camera
use std.ui.gfx.term as terminal
use std.math.matrix *
use std.math.vector *
use std.str as str
use std.str *
use std.util.common as common

def APP_MSAA = 8
mut APP_BG   = [0.002, 0.002, 0.005, 1.0]
mut APP_WIRE = false
mut APP_STATS= true
mut APP_RAINBOW = 0.8
def COMMANDS = ["teapot", "ortho", "persp", "clear", "exit", "help", "tp", "fov", "speed", "sens", "bg", "pos", "rot", "wireframe", "msaa", "stats", "shader", "rainbow", "snapshot"]

def WHITE = [1.0, 1.0, 1.0, 1.0]
def WHITE_U32 = color_pack(1.0, 1.0, 1.0, 1.0)
def HUD_BG_U32   = color_pack(0.01, 0.01, 0.02, 0.9)

def CUSTOM_VERT_SRC = "#version 450
layout(location=0) in vec3 pos ;
layout(location=1) in vec2 uv ;
layout(location=2) in vec4 color ;
layout(location=3) in uint texIndex ;
layout(location=4) in vec3 normal ;
layout(location=0) out vec4 vColor ;
layout(location=1) out vec3 vNormal ;
layout(location=2) out vec3 vWorldPos ;
layout(push_constant) uniform PC { mat4 vp ; mat4 model; int isMask; int isUnlit; float time; float rainbow; vec3 eyePos; } pc;
void main(){
  vec4 worldPos = pc.model * vec4(pos, 1.0) ;
  vWorldPos = worldPos.xyz ;
  vNormal = mat3(pc.model) * normal ;
  vColor = color ;
  gl_Position = pc.vp * worldPos ;
}
"
def CUSTOM_FRAG_SRC = "#version 450
layout(location=0) in vec4 vColor ;
layout(location=1) in vec3 vNormal ;
layout(location=2) in vec3 vWorldPos ;
layout(push_constant) uniform PC { mat4 vp ; mat4 model; int isMask; int isUnlit; float time; float rainbow; vec3 eyePos; } pc;
layout(location=0) out vec4 outColor ;
void main(){
  vec3 n = normalize(vNormal) ;
  bool isBack = !gl_FrontFacing ;
  if(isBack) n = -n ;

  vec3 l = normalize(vec3(0.5, 2.0, 0.5)) ;
  vec3 v = normalize(pc.eyePos - vWorldPos + vec3(0.00001)) ;
  vec3 r = reflect(-l, n) ;

  float diff = max(dot(n, l), 0.0) ;
  float rim = pow(1.0 - max(dot(v, n), 0.0), 4.0) ;
  float spec = pow(max(dot(r, v), 0.0), 32.0) ;

  vec3 skyCol = vec3(0.1, 0.3, 0.7) ;
  vec3 groundCol = vec3(0.05, 0.02, 0.01) ;
  vec3 ambient = mix(groundCol, skyCol, n.y * 0.5 + 0.5) * 0.2 ;

  vec3 rb_col = 0.5 + 0.5 * cos(pc.time * 2.0 + n.xyz * 3.0 + vec3(0,2,4)) ;
  vec3 base_color = mix(vColor.rgb, rb_col, pc.rainbow) ;

  vec3 rim_glow = rb_col * rim * (0.4 + 0.6 * abs(sin(pc.time * 0.8))) ;

  vec3 final = base_color * (diff * 0.7 + ambient) + spec * 0.5 + rim_glow * 0.8 ;

  if(isBack) final *= 0.15 ;

  float ground_occ = smoothstep(-20.0, 60.0, vWorldPos.y) ;
  outColor = vec4(clamp(final * ground_occ, 0.0, 1.0), vColor.a) ;
}
"

mut win         = 0
mut cam         = 0
mut cam3d       = 0
mut fps         = 0
mut frame_num   = 0
mut start_t     = 0
mut last_upd_t  = 0
mut last_fps_t  = 0
mut last_draw_ms = 0.0
mut last_flush_ms = 0.0
mut is_ortho     = false
mut show_teapot  = false
mut skip_mouse_frames = 0
mut total_frames = 0
mut timeout_ns   = 0

mut _cube_mesh = 0
mut _grid_mesh = 0

mut res_font    = 0
mut res_tex     = 0
mut teapot      = 0
mut custom_pipe = 0
mut active_shader = true
mut res_cube    = 0
mut res_grid    = 0
mut res_pc_data = 0

mut _cube_ptr  = 0 mut _cube_cnt = 0
mut _grid_ptr  = 0 mut _grid_cnt = 0

mut _win_w = 1280.0 mut _win_h = 720.0
mut _last_mx = 0.0 mut _last_my = 0.0 mut _mouse_init = 0

mut _cached_fps = -1 mut _cached_fps_str = ""
mut _diag_texts = [""]
mut _color_diag = 0 mut _color_fps_g = 0 mut _color_fps_w = 0 mut _color_fps_b = 0
mut _color_perf = 0 mut _color_cross = 0

mut _prof_mode = 0
mut _prof_history = []
mut _prof_t_update = 0.0
mut _prof_t_draw   = 0.0
mut _prof_t_flush  = 0.0
mut _prof_t_frame  = 0.0
mut _last_frame_ticks = 0

def M_ID = mat4_identity()
def M_RX = mat4_identity()
def M_RY = mat4_identity()
def M_RZ = mat4_identity()
def M_T  = mat4_identity()
def M_S  = mat4_identity()
def M_W  = mat4_identity()
def M_tmp = mat4_identity()
def M_PT  = mat4_identity()
def M_PR  = mat4_identity()
def M_PS  = mat4_identity()
def M_PW  = mat4_identity()
def M_Ptmp = mat4_identity()
def M_V   = mat4_identity()
def M_P   = mat4_identity()
def M_VP  = mat4_identity()
def M_UP  = [0.0, 1.0, 0.0]

fn exec_cmd(line){
   "Executes a console command."
   def parts = str.split(line, " ")
   def cmd = get(parts, 0)
   if(eq(cmd, "teapot") || eq(cmd, "tp")){ show_teapot = !show_teapot }
   elif(eq(cmd, "ortho")){ is_ortho = true }
   elif(eq(cmd, "persp")){ is_ortho = false }
   elif(eq(cmd, "clear")){ terminal.clear() }
   elif(eq(cmd, "snapshot")){
      mut path = "snapshot.tga"
      def space_idx = str.find(line, " ")
      if(space_idx >= 0){
         path = str.strip(str.str_slice(line, space_idx + 1, len(line)))
      }
      if(len(path) == 0){ path = "snapshot.tga" }
      if(snapshot(path)){
         terminal.log("Snapshot saved to " + path)
      } else {
         terminal.log("ERROR: Failed to save snapshot to " + path)
      }
   }
   elif(eq(cmd, "exit")){ window.set_should_close(win, true) }
   elif(eq(cmd, "fov")){ if(len(parts) > 1){ camera.set_fov(cam3d, str.atof(get(parts, 1))) } }
   elif(eq(cmd, "speed")){ if(len(parts) > 1){ camera.set_speed(cam3d, str.atof(get(parts, 1))) } }
   elif(eq(cmd, "sens")){ if(len(parts) > 1){ camera.set_sens(cam3d, str.atof(get(parts, 1))) } }
   elif(eq(cmd, "bg")){
      if(len(parts) >= 4){ APP_BG = [str.atof(get(parts,1)), str.atof(get(parts,2)), str.atof(get(parts,3)), 1.0] }
   }
   elif(eq(cmd, "pos")){
      def p = camera.get_pos(cam3d)
      terminal.log(f"POS: {get(p,0):.2f}, {get(p,1):.2f}, {get(p,2):.2f}")
   }
   elif(eq(cmd, "rot")){
      def r = camera.get_rot(cam3d)
      terminal.log(f"ROT: Yaw={get(r,0):.1f} Pitch={get(r,1):.1f}")
   }
   elif(eq(cmd, "wireframe")){ APP_WIRE = !APP_WIRE set_wireframe(APP_WIRE) }
   elif(eq(cmd, "stats")){ APP_STATS = !APP_STATS }
   elif(eq(cmd, "shader")){ active_shader = !active_shader terminal.log(active_shader ? "Shader: CUSTOM" : "Shader: DEFAULT") }
   elif(eq(cmd, "rainbow")){ if(len(parts) > 1){ APP_RAINBOW = str.atof(get(parts, 1)) } else { APP_RAINBOW = (APP_RAINBOW > 0.0 ? 0.0 : 0.8) } }
   elif(eq(cmd, "prof")){ if(len(parts) > 1){ _prof_mode = int(str.atof(get(parts, 1))) } else { _prof_mode = (_prof_mode > 0 ? 0 : 1) } terminal.log(f"Profiling graph: {_prof_mode}") }
   elif(eq(cmd, "help")){ terminal.log("CMD: teapot, ortho, persp, clear, snapshot, exit, fov, speed, sens, bg, pos, rot, wireframe, stats, shader, rainbow, prof") }
}

fn build_cube(s, color, tex_id=0){
   "Builds a textured 3D cube mesh."
   def buf = sys_malloc(36 * VERTEX_STRIDE)
   def c = color_pack(get(color,0), get(color,1), get(color,2), get(color,3))
   def _v = fn(i, x, y, z, u, v, nx, ny, nz){ push_vertex(buf + i * VERTEX_STRIDE, x, y, z, u, v, c, tex_id, nx, ny, nz) }
   _v(0, -s,-s, s, 0,0, 0,0, 1) _v(1,  s,-s, s, 1,0, 0,0, 1) _v(2,  s, s, s, 1,1, 0,0, 1)
   _v(3, -s,-s, s, 0,0, 0,0, 1) _v(4,  s, s, s, 1,1, 0,0, 1) _v(5, -s, s, s, 0,1, 0,0, 1)
   _v(6,  s,-s,-s, 0,0, 0,0,-1) _v(7, -s,-s,-s, 1,0, 0,0,-1) _v(8, -s, s,-s, 1,1, 0,0,-1)
   _v(9,  s,-s,-s, 0,0, 0,0,-1) _v(10,-s, s,-s, 1,1, 0,0,-1) _v(11, s, s,-s, 0,1, 0,0,-1)
   _v(12,-s, s, s, 0,0, 0,1,0) _v(13, s, s, s, 1,0, 0,1,0) _v(14, s, s,-s, 1,1, 0,1,0)
   _v(15,-s, s, s, 0,0, 0,1,0) _v(16, s, s,-s, 1,1, 0,1,0) _v(17,-s, s,-s, 0,1, 0,1,0)
   _v(18,-s,-s,-s, 0,0, 0,-1,0) _v(19, s,-s,-s, 1,0, 0,-1,0) _v(20, s,-s, s, 1,1, 0,-1,0)
   _v(21,-s,-s,-s, 0,0, 0,-1,0) _v(22, s,-s, s, 1,1, 0,-1,0) _v(23,-s,-s, s, 0,1, 0,-1,0)
   _v(24, s,-s, s, 0,0, 1,0,0) _v(25, s,-s,-s, 1,0, 1,0,0) _v(26, s, s,-s, 1,1, 1,0,0)
   _v(27, s,-s, s, 0,0, 1,0,0) _v(28, s, s,-s, 1,1, 1,0,0) _v(29, s, s, s, 0,1, 1,0,0)
   _v(30,-s,-s,-s, 0,0, -1,0,0) _v(31,-s,-s, s, 1,0, -1,0,0) _v(32,-s, s, s, 1,1, -1,0,0)
   _v(33,-s,-s,-s, 0,0, -1,0,0) _v(34,-s, s, s, 1,1, -1,0,0) _v(35,-s, s,-s, 0,1, -1,0,0)
   mut d = dict() d = dict_set(d, "ptr", buf) d = dict_set(d, "cnt", 36) d
}

fn startup(){
   "Initializes the application state, window, and resources."
   win = init_window(1280, 720, "Nytrix", 0, false, false, APP_MSAA)

   def fpaths = [
      "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf",
      "/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Regular.ttf",
      "/usr/share/fonts/TTF/FiraCode-Regular.ttf",
      "etc/assets/font/jetbrains.ttf",
      "etc/assets/font/monocraft.ttf",
      "/usr/share/fonts/TTF/DejaVuSansMono.ttf"
   ]
   mut i = 0 while(i < len(fpaths)){
      def p = get(fpaths, i)
      res_font = font_load(p, 14)
      if(res_font){ break }
      i += 1
   }

   res_tex  = texture_load("etc/assets/images/test.png")
   teapot   = mesh_load("etc/assets/models/teapot.obj", [0.8, 0.4, 0.2, 1.0])
   res_cube = build_cube(6.0, WHITE, res_tex)
   res_grid = build_env()
   res_pc_data = sys_malloc(128)

   cam = camera_init([0.0, 0.0, 0.0], 0.0, 0.0)
   cam3d = camera.init([0.0, 8.0, 45.0], 0.0, 0.0)
   set_idx(cam3d, 13, 1.25) ; Rot smooth
   set_idx(cam3d, 14, 10.0) ; Move smooth

   custom_pipe = create_pipeline(compile_shader(CUSTOM_VERT_SRC, "vert"), compile_shader(CUSTOM_FRAG_SRC, "frag"), 3, 1, 1, 0, 0, 0, 0)

   terminal.init(res_font, HUD_BG_U32, WHITE_U32)
   start_t = ticks()
   last_upd_t = start_t
   last_fps_t = start_t
   terminal.log("ターミナル 🫪.")

   _cube_ptr = dict_get(res_cube, "ptr", 0) _cube_cnt = dict_get(res_cube, "cnt", 0)
   _grid_ptr = dict_get(res_grid, "ptr", 0) _grid_cnt = dict_get(res_grid, "cnt", 0)

   _grid_mesh = mesh_create(_grid_ptr, _grid_cnt, false)
   _cube_mesh = mesh_create(_cube_ptr, _cube_cnt, false)

   _color_diag = color_pack(0.5, 0.6, 0.7, 0.8)
   _color_fps_g = color_pack(0.1, 1.0, 0.4, 0.9)
   _color_fps_w = color_pack(1.0, 0.7, 0.0, 0.9)
   _color_fps_b = color_pack(1.0, 0.2, 0.1, 1.0)
   _color_perf = color_pack(0.7, 0.9, 1.0, 0.7)

   def env_t = env("NY_UI_TIMEOUT")
   if(env_t){ timeout_ns = int(str.atof(env_t) * 1e9) }

   window.set_cursor_mode(win, window.CURSOR_LOCKED)
   set_clear_color(APP_BG)
}

fn build_env(){
   "Builds the infinite grid floor mesh using solid quads to bypass hardware limits."
   def radius = 200
   def spacing = 5.0
   def size = float(radius) * spacing
   def buf = sys_malloc((radius * 2 + 1) * 12 * VERTEX_STRIDE)
   def c = color_pack(0.12, 0.15, 0.18, 1.0)
   def w = 0.04
   mut off = 0 mut gi = -radius while(gi <= radius){
      def v = float(gi) * spacing
      push_vertex(buf+off, -size, 0, v-w, 0,0,c) off+=VERTEX_STRIDE
      push_vertex(buf+off,  size, 0, v-w, 0,0,c) off+=VERTEX_STRIDE
      push_vertex(buf+off, -size, 0, v+w, 0,0,c) off+=VERTEX_STRIDE
      push_vertex(buf+off,  size, 0, v-w, 0,0,c) off+=VERTEX_STRIDE
      push_vertex(buf+off,  size, 0, v+w, 0,0,c) off+=VERTEX_STRIDE
      push_vertex(buf+off, -size, 0, v+w, 0,0,c) off+=VERTEX_STRIDE

      push_vertex(buf+off, v-w, 0, -size, 0,0,c) off+=VERTEX_STRIDE
      push_vertex(buf+off, v+w, 0, -size, 0,0,c) off+=VERTEX_STRIDE
      push_vertex(buf+off, v-w, 0,  size, 0,0,c) off+=VERTEX_STRIDE
      push_vertex(buf+off, v+w, 0, -size, 0,0,c) off+=VERTEX_STRIDE
      push_vertex(buf+off, v+w, 0,  size, 0,0,c) off+=VERTEX_STRIDE
      push_vertex(buf+off, v-w, 0,  size, 0,0,c) off+=VERTEX_STRIDE
      gi += 1
   }
   mut d = dict() d = dict_set(d, "ptr", buf) d = dict_set(d, "cnt", (radius * 2 + 1) * 12) d
}

fn update(dt){
   "Main update loop handle: processes events, camera, and input."
   def t0_up = ticks()

   mut e = window.check_event(win)
   while(e != 0){
      def typ = window.event_type(e) def data = window.event_data(e)

      if(typ == EVENT_KEY_PRESSED){
         def k = dict_get(data, "key", 0)
         if(k == uin.KEY_GRAVE || k == uin.KEY_F1){
         terminal.toggle(win)
         skip_mouse_frames = 2
         }
      }

      mut res = terminal.handle_event(typ, data)
      if(res == 2){ terminal.exec(exec_cmd) }
      if(!res && typ == EVENT_QUIT){ window.set_should_close(win, true) }

      if(typ == EVENT_WINDOW_RESIZED){
          _win_w = float(dict_get(data, "w", 1280.0))
          _win_h = float(dict_get(data, "h", 720.0))
          set_win_size(_win_w, _win_h)
      } elif(typ == EVENT_MOUSE_SCROLL){
          def dy = float(dict_get(data, "dy", 0.0))
          if(!terminal.is_open()){
             def n_fov = clamp(get(cam3d, 16) - dy * 2.5, 15.0, 120.0)
             set_idx(cam3d, 16, n_fov)
          }
      }
      e = window.check_event(win)
   }
   if(window.key_pressed(win, uin.KEY_ESCAPE)){ window.set_should_close(win, true) }

   if(!terminal.is_open()){
      def skip_look = skip_mouse_frames > 0
      if(skip_look){ skip_mouse_frames -= 1 }

      def mpos = window.cursor_pos(win)
      mut mx = get(mpos, 0, 0.0) mut my = get(mpos, 1, 0.0)

      if(_mouse_init == 0){ _last_mx = mx _last_my = my _mouse_init = 1 }

      def dx = mx - _last_mx
      def dy = my - _last_my
      _last_mx = mx _last_my = my

      def sens = get(cam3d, 10) * 4.25
      if(!skip_look){
         if(abs(dx) > 0.0001 || abs(dy) > 0.0001){
         set_idx(cam3d, 8, get(cam3d, 8) + dx * sens * dt * 60.0) ; Scale by delta time
         set_idx(cam3d, 9, clamp(get(cam3d, 9) - dy * sens * dt * 60.0, -89.9, 89.9))
         }
      }
      def rs = 1.0 - exp(0.0 - (get(cam3d, 13) * dt))
      mut cyaw = get(cam3d, 6) mut cpitch = get(cam3d, 7)
      cyaw += (get(cam3d, 8) - cyaw) * rs
      cpitch += (get(cam3d, 9) - cpitch) * rs
      set_idx(cam3d, 6, cyaw) set_idx(cam3d, 7, cpitch)

      def ry = cyaw * PI / 180.0
      def sinr = sin(ry) def cosr = cos(ry)
      def fwd_x = sinr def fwd_z = 0.0 - cosr
      def rgt_x = cosr def rgt_z = sinr

      mut wx = 0.0 mut wy = 0.0 mut wz = 0.0
      if(window.key_down(win, uin.KEY_W)){ wx += fwd_x wz += fwd_z }
      if(window.key_down(win, uin.KEY_S)){ wx -= fwd_x wz -= fwd_z }
      if(window.key_down(win, uin.KEY_A)){ wx -= rgt_x wz -= rgt_z }
      if(window.key_down(win, uin.KEY_D)){ wx += rgt_x wz += rgt_z }
      if(window.key_down(win, uin.KEY_SPACE)){ wy += 1.0 }
      if(window.key_down(win, uin.KEY_CTRL)){ wy -= 1.0 }

      def wlen2 = wx*wx + wy*wy + wz*wz
      if(wlen2 > 0.0001){ def inv = 1.0 / sqrt(wlen2) wx *= inv wy *= inv wz *= inv }

      def ms = 1.0 - exp(0.0 - (get(cam3d, 14) * dt))
      mut cx2 = get(cam3d, 17) mut cy2 = get(cam3d, 18) mut cz = get(cam3d, 19)
      cx2 += (wx - cx2) * ms cy2 += (wy - cy2) * ms cz  += (wz - cz)  * ms
      set_idx(cam3d, 17, cx2) set_idx(cam3d, 18, cy2) set_idx(cam3d, 19, cz)

      mut speed = get(cam3d, 11)
      if(window.key_down(win, uin.KEY_SHIFT)){ speed *= get(cam3d, 15) }

      def drag_f = 1.0 / (1.0 + get(cam3d, 12) * dt)
      mut vx = get(cam3d, 3) mut vy = get(cam3d, 4) mut vz = get(cam3d, 5)
      vx = (vx + cx2 * speed * dt) * drag_f
      vy = (vy + cy2 * speed * dt) * drag_f
      vz = (vz + cz  * speed * dt) * drag_f
      set_idx(cam3d, 3, vx) set_idx(cam3d, 4, vy) set_idx(cam3d, 5, vz)

      set_idx(cam3d, 0, get(cam3d, 0) + vx * dt)
      set_idx(cam3d, 1, get(cam3d, 1) + vy * dt)
      set_idx(cam3d, 2, get(cam3d, 2) + vz * dt)
   }

   mut cxaw = get(cam3d, 6) mut cpch = get(cam3d, 7)
   def ryaw = cxaw * PI / 180.0 def rpch = cpch * PI / 180.0
   def cp = cos(rpch)
   def fx = sin(ryaw) * cp def fy = sin(rpch) def fz = -cos(ryaw) * cp

   def px = get(cam3d, 0) def py = get(cam3d, 1) def pz = get(cam3d, 2)
   def tx = px + fx def ty = py + fy def tz = pz + fz

   mut g_pos = get(cam, 0) g_pos[0] = px g_pos[1] = py g_pos[2] = pz
   mut g_tgt = get(cam, 1) g_tgt[0] = tx g_tgt[1] = ty g_tgt[2] = tz
   set_idx(cam, 4, cxaw) set_idx(cam, 5, cpch)

   def aspect = _win_w / _win_h
   mat4_look_at_into_xyz(px, py, pz, tx, ty, tz, 0.0, 1.0, 0.0, M_V)
   if(is_ortho){
      def sz = 40.0
      mat4_ortho_into(0-sz*aspect, sz*aspect, 0-sz, sz, 0.1, 1000.0, M_P)
   } else {
      mat4_perspective_into(get(cam3d, 16) * PI / 180.0, aspect, 0.1, 1000.0, M_P)
   }
   mat4_mul_into(M_P, M_V, M_VP)

   _prof_t_update = float(ticks() - t0_up) / 1e6
}

fn draw(phase){
   "Main draw loop handle: records 3D and 2D rendering commands."
   def t0_dr = ticks()
   set_view_proj(M_VP)

   def snp = 5.0
   def cam_x = get(cam3d, 0)
   def cam_z = get(cam3d, 2)
   def snx = fsub(cam_x, fmod(cam_x, snp))
   def snz = fsub(cam_z, fmod(cam_z, snp))
   mat4_translate_into(snx, 0.0, snz, M_tmp)

   set_unlit(true) set_model_matrix(M_tmp)
   draw_mesh(_grid_mesh, false)
   set_model_matrix(M_ID)

   draw_line_3d(0, 0, 0, 40, 0, 0, 0.04, 1.0, 0.0, 0.0, 1.0) ; X - Red
   draw_line_3d(0, 0, 0, 0, 40, 0, 0.04, 0.0, 1.0, 0.0, 1.0) ; Y - Green
   draw_line_3d(0, 0, 0, 0, 0, 40, 0.04, 0.0, 0.0, 1.0, 1.0) ; Z - Blue

   mat4_rotate_x_into(phase * 0.5, M_RX)
   mat4_rotate_y_into(phase * 0.6, M_RY)
   mat4_rotate_z_into(phase * 0.7, M_RZ)
   mat4_mul_into(M_RX, M_RY, M_tmp)
   mat4_mul_into(M_tmp, M_RZ, M_W)
   set_model_matrix(M_W)

   set_unlit(false)
   texture_bind(res_tex)
   draw_mesh(_cube_mesh)
   texture_bind(-1)

   if(teapot && show_teapot){
      mat4_translate_into(0, 10, 0, M_PT)
      mat4_rotate_y_into(phase * 1.5, M_PR)
      mat4_scale_into(6, 6, 6, M_PS)
      mat4_mul_into(M_PT, M_PR, M_Ptmp)
      mat4_mul_into(M_Ptmp, M_PS, M_PW)
      set_model_matrix(M_PW)

      if(custom_pipe && active_shader){
         bind_pipeline(custom_pipe)
         store32_f32(res_pc_data, phase, 0)
         store32_f32(res_pc_data, APP_RAINBOW, 4)
         def p = get(cam, 0)
         store32_f32(res_pc_data, float(get(p,0)), 8)
         store32_f32(res_pc_data, float(get(p,1)), 12)
         store32_f32(res_pc_data, float(get(p,2)), 16)
         push_constants(res_pc_data, 20, 136)
      } else { set_unlit(false) }

      draw_mesh(teapot)
      if(custom_pipe && active_shader){ reset_pipeline() }
   }

   _prof_t_draw = float(ticks() - t0_dr) / 1e6

   def ww = _win_w def wh = _win_h
   clear_depth() set_ortho_2d(0, ww, 0, wh)
   set_unlit(true) set_model_matrix(M_ID)
   if(terminal.is_open()){
      terminal.draw(ww, wh)
   }

   if(APP_STATS){
      def x = ww - 110.0 def y = 12.0
      draw_rect_fast(x - 8, y - 4, 100.0, 22.0, 0xCC050508)
      draw_rect_fast(x - 8, y - 4, 100.0, 1.0, 0x9955FFCC)
      def fv = fps
      if(fv != _cached_fps){
         _cached_fps = fv
         _cached_fps_str = f"FPS {fv:04}"
         _diag_texts[0] = _cached_fps_str
      }
      def col_fps = (fv >= 100) ? _color_fps_g : ((fv >= 50) ? _color_fps_w : _color_fps_b)
      draw_text_batch(res_font, _diag_texts, x, y, 14, col_fps)
      if(int(phase * 4) % 2 == 0){ draw_rect_fast(x + 75, y + 4, 5, 5, 0xCC55FFCC) }
   }

   if(_prof_mode > 0){
      def gx = 30.0 def gy = wh - 150.0 def gw = 300.0 def gh = 100.0
      draw_rect_fast(gx, gy, gw, gh, 0xAA020202)
      draw_line_strip_2d(gx, gy, gw, gh, _prof_history, 100.0, 0.4, 1.0, 0.6, 1.0)
      draw_text(res_font, f"FRAME TIME: {1000.0/float(fps):.2f}", gx, gy - 20, 0xFFFFFFFF)
   }

   if(!terminal.is_open()){
      def cx = ww * 0.5 def cy = wh * 0.5
      mut ix = int(cx) mut iy = int(cy)
      ix = ix - 1 iy = iy - 1
      draw_rect(ix,     iy,     2, 2, 1, 1, 1, 0.7) ; Center
      draw_rect(ix,     iy-8,   2, 4, 1, 1, 1, 0.4) ; Top
      draw_rect(ix,     iy+6,   2, 4, 1, 1, 1, 0.4) ; Bottom
      draw_rect(ix-8,   iy,     4, 2, 1, 1, 1, 0.4) ; Left
      draw_rect(ix+6,   iy,     4, 2, 1, 1, 1, 0.4) ; Right
   }

   if(_prof_mode > 0){
      _prof_t_flush = float(ticks() - t0_dr) / 1e6 - _prof_t_draw
      def t_total = _prof_t_update + _prof_t_draw + _prof_t_flush
      _prof_history = append(_prof_history, t_total * 0.5)
      if(len(_prof_history) > 300){ _prof_history = slice(_prof_history, 1, 301) }
   }
}

mut _render_phase = 0.0
mut _render_dt = 0.0
mut _running = true

fn render_thread_obj(){
   "Off-thread rendering loop: handles GPU submission and synchronization."
   while(_running){
      def phase = _render_phase
      if(phase < 0.0){ os.time.sleep_ms(0) continue }

      def tc0 = ticks()
      begin_frame()
      _t_clear += ticks() - tc0

      def t0 = ticks()
      draw(phase)
      def t1 = ticks()
      _t_draw += t1 - t0

      end_frame()
      def t2 = ticks()
      _t_flush += t2 - t1
      last_draw_ms = float(t1 - t0) / 1e6 last_flush_ms = float(t2 - t1) / 1e6

      _render_phase = -1.0
   }
}

mut _t_update = 0 mut _t_clear = 0 mut _t_draw = 0 mut _t_flush = 0
startup()
mut _elapsed = 0

def r_thread = thread_spawn(render_thread_obj)

while(!window.should_close(win)){
   def now = ticks() mut dt = float(now - last_upd_t) / 1e9
   if(dt > 0.1){ dt = 0.016 } if(dt < 0.0001){ dt = 0.0001 } last_upd_t = now
   _elapsed = now - start_t
   if(timeout_ns > 0 && _elapsed >= timeout_ns){ window.set_should_close(win, true) }

   def tu0 = ticks()
   update(dt)
   _t_update += ticks() - tu0

   while(_render_phase >= 0.0){
      os.time.sleep_ms(0)
   }
   _render_dt = dt
   _render_phase = float(_elapsed) / 1e9

   frame_num += 1 total_frames += 1
   if(now - last_fps_t >= 1e9){ fps = frame_num frame_num = 0 last_fps_t = now }
}
_running = false
thread_join(r_thread)
if(env("NY_UI_FPS_LOG") && total_frames > 0){
   def elapsed_s = float(ticks() - start_t) / 1e9
   def n = float(total_frames)
   def avg = int(n / elapsed_s)
   def us = fn(x){ float(x) / n / 1000.0 }
   print(f"[FPS] avg={avg}  frames={total_frames}  time={elapsed_s:.2f}s")
   print(f"  update: {us(_t_update):.3f}ms/f  clear: {us(_t_clear):.3f}ms/f  draw: {us(_t_draw):.3f}ms/f  flush: {us(_t_flush):.3f}ms/f")
}
window.set_cursor_mode(win, window.CURSOR_NORMAL)
exit(0)
