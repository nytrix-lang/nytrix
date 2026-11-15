;; Focused text API; the render facade remains compatible with existing apps.
module core.os.ui.render.text(
   FONT_FILTER_DEFAULT, FONT_FILTER_NEAREST, FONT_FILTER_LINEAR,
   FONT_FILTER_BILINEAR, font_load, font_load_first, font_destroy,
   font_prepare, font_line_height, font_ascent, draw_text, draw_text_3d,
   draw_text_centered, draw_text_centered_x, draw_text_pair,
   draw_text_batch, draw_text_runs, draw_text_runs_flat,
   draw_text_runs_flat_colors, measure_text, measure_text_fast
)

use core.os.ui.render (
   FONT_FILTER_DEFAULT, FONT_FILTER_NEAREST, FONT_FILTER_LINEAR,
   FONT_FILTER_BILINEAR, font_load, font_load_first, font_destroy,
   font_prepare, font_line_height, font_ascent, draw_text, draw_text_3d,
   draw_text_centered, draw_text_centered_x, draw_text_pair,
   draw_text_batch, draw_text_runs, draw_text_runs_flat,
   draw_text_runs_flat_colors, measure_text, measure_text_fast
)
