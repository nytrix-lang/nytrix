use test.runtime.imports.diamond.d as d
module b ( val )
fn val(){ return d.val() }
