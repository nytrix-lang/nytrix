use test.runtime.imports.diamond.d as d
module c ( val )
fn val(){ return d.val() }
