use std.core
use std
use std math as loose_math
use std.core.iter
use std.core.str as str
use std.math.bin as bin
use std.math.scalar as scalar
use std.math.nt (Z, bigint_abs, bigint_to_str)
use std.math.crypto.encoding
use std.math.crypto as cryptoroot
use std.os (waitpid as os_waitpid)
use std.os.args as os_args
use "./use/local.ny" (helper_val, helper_add)
use "./use/local.ny" (helper_add as add2)
use "./use/declared.ny" as declared_provider
use "./use/declared.ny" (make_report as declared_make_report)
use "./use/collision/provider.ny" as collision_a
use "./use/guard.ny" (main_guard_runtime_seen, main_guard_comptime_seen)
use "./use/implicitpkg.ny" as implicitpkg
use "./use/implicitpkg.ny" (child_value as implicit_child_value)
use "./use/implicitpkg.ny"
module ReExportMath(
   ceil, floor
){
   use std.math.float (ceil, floor)
}

module ReExportScalar(
   ceil, PI
){
   use std.math.scalar (ceil, PI)
}

module ReExportChain(
   ceil, PI
){
   use ReExportScalar (ceil, PI)
}

use ReExportMath as remath
use ReExportMath (floor as re_floor)
use ReExportChain as chain_math
use ReExportChain (ceil as chain_ceil, PI as CHAIN_PI)

fn flag_from(vals): str { "shadow:" + to_str(vals.len) }
assert(helper_val() == 123, "relative import value")
assert(helper_add(19, 23) == 42, "relative import function")
assert(add2(20, 22) == 42, "relative import alias")
def declared_typed = declared_provider.make_report().set("tag", "typed")
assert(declared_typed.get("ok", false), "local path alias resolves declared module name")
assert(declared_typed.get("tag", "") == "typed", "local path alias supports chained returned dict methods")
assert(declared_make_report().get("ok", false), "local path named import resolves declared module name")
def declared_dynamic = declared_provider.make_report_any().set("tag", "dynamic")
assert(declared_dynamic.get("ok", false), "local path alias resolves declared module any return")
assert(declared_dynamic.get("tag", "") == "dynamic", "local path alias supports dynamic returned dict chain")
assert(collision_a.collision_marker() == "a", "relative path use resolves from importing file, not suffix")
assert(!main_guard_runtime_seen(), "runtime __main() is false in imported files")
assert(!main_guard_comptime_seen(), "comptime __main() is false in imported files")
assert(implicitpkg.parent_value() == 7, "explicit parent module function resolves")
assert(implicitpkg.parent_child_sum() == 84, "parent module uses exported child without a child use line")
assert(implicitpkg.child.child_value() == 42, "exported child module loads without repeated use")
assert(implicitpkg.child.grand.grand_value() == 99, "exported grandchild module loads without repeated use")
assert(implicitpkg.child.child_grand_sum() == 141, "exported child module reaches its exported child surface")
assert(implicitpkg.child_value() == 42, "parent module exposes exported child public surface")
assert(implicit_child_value() == 42, "named import resolves through exported child public surface")
assert(child.child_label() == "implicit-child", "import-all exposes exported child module alias")
assert(child_label() == "implicit-child", "import-all exposes exported child functions")
assert(all([1, 2, 3], fn(v){ v > 0 }), "bare use imports exported names")
assert(iter.any([1, 2, 3], fn(v){ v > 2 }), "bare use keeps module alias")
assert(math.abs(-9) == 9, "std root use exposes math namespace")
assert(std.math.abs(-11) == 11, "std root use exposes full std namespace")
assert(str.split("alpha,beta", ",").get(0) == "alpha", "std root use exposes str namespace")
assert(os.file_exists(os.getcwd()) == true, "std root use exposes os namespace")
assert(nt.is_prime(17) == true, "std root use exposes nested nt namespace")
assert(loose_math.abs(-12) == 12, "loose std module use normalizes in real compiler")
assert(to_str(123) == "123", "core to_str survives aliased imports")
assert(load8("abc", 0) == 97, "core load8 survives aliased imports")
assert(str.utf8_slice("abcdef", 1, 3, 1) == "bc", "aliased utf8_slice works")
assert(type(scalar.PI) == "float", "direct module alias global keeps boxed float type")
assert(scalar.PI > 3.0, "direct module alias resolves global")
assert(scalar.ceil(1.2) == 2, "direct module alias resolves function")
assert(bigint_to_str(bigint_abs(Z(-3))) == "3", "re-exported function resolves source-local helper")
assert(remath.ceil(1.2) == 2, "module alias resolves re-exported import")
assert(re_floor(1.8) == 1, "named use resolves re-exported import")
assert(chain_math.ceil(1.2) == 2, "module alias resolves chained re-exported import")
assert(chain_ceil(2.1) == 3, "named use resolves chained re-exported import")
assert(CHAIN_PI > 3.0, "named use resolves chained re-exported global")
assert(type(chain_math.PI) == "float", "module alias global keeps boxed float type")
assert(chain_math.PI > 3.0, "module alias resolves chained re-exported global")
assert(hex_encode([110, 121]) == "6e79", "package use exposes exported child functions without local use fanout")
assert(encoding.hex_encode([110, 121]) == "6e79", "package use exposes exported child module function through namespace")
assert(scream.scream_decode_marks([0x300]) == "V", "package use exposes exported child module alias")
assert(cryptoroot.encoding.hex_encode([110, 121]) == "6e79", "root crypto alias exposes encoding namespace")
def b = bin.hex_to_bytes("f6fa2d")
assert(b.len == 3, "bytes/list length after aliased imports")
assert(get(b, 0) == 246, "generic get after aliased imports")
assert(b.get(0) == 246, "method get after aliased imports")
assert(min(3, 7) == 3, "core min is available")
assert(max(3, 7) == 7, "core max is available")
def any: waitpid_fn = os_waitpid
assert(type(waitpid_fn) == "ptr", "std.os waitpid facade imports as function pointer")
assert(!os_args.flag("--definitely-not-present"), "module wrapper resolves module-local helper under user shadow")
mut a = [10, 20]
a[1] = 99
assert(a.get(1) == 99, "index assignment lowers through set_idx")
print("✓ import tests passed")
