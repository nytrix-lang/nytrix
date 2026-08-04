;; Keywords: browser portability process negative fixture
;; Browser targets must reject native process spawning instead of substituting a host fallback.
use std.os.process

process.run("true", [])
