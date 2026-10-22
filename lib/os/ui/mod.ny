;; Keywords: ui window event input keyboard mouse gamepad render vulkan camera scene assets texture mesh font terminal profiling dump selection os
;; UI facade for windowing, events, input, rendering support, cameras, scene assets, profiling, and probes.
;; References:
;; - core.os
module core.os.ui(window, event, consts, shader_mod, gamepad, viewer, batch, asset_batch, bootstrap, loop, idle, reuse, camera, scene, assets, dump, selection, profile, app, diag)
use core.os.ui.window as window
use core.os.ui.window.event as event
use core.os.ui.window.consts as consts
use core.os.ui.render.shader as shader_mod
use core.os.ui.render.camera as camera
use core.os.ui.render.scene as scene
use core.os.ui.assets as assets
use core.os.ui.render.dump as dump
use core.os.ui.render.viewer.engine.selection as selection
use core.os.ui.render.dump as profile
use core.os.ui.render.diag as diag
use core.os.ui.render.viewer.app as app
use core.os.ui.window.input.gamepad as gamepad
use core.os.ui.render.viewer as viewer
use core.os.ui.render.viewer.batch as batch
use core.os.ui.assets.batch as asset_batch
use core.os.ui.render.viewer.bootstrap as bootstrap
use core.os.ui.render.viewer.loop as loop
use core.os.ui.render.viewer.idle as idle
use core.os.ui.render.reuse as reuse
