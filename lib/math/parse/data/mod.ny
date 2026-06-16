;; Keywords: data serialization zlib deflate compression csv delimited xml json yaml yml toml sql parse
;; Data-format facade for JSON, YAML, TOML, CSV, XML, SQL inspection, and zlib compression.
;; References:
;; - std.math.parse
module std.math.parse.data(json, yaml, toml, zlib, csv, xml, sql)
use std.math.parse.data.json as json
use std.math.parse.data.yaml as yaml
use std.math.parse.data.toml as toml
use std.math.parse.data.zlib as zlib
use std.math.parse.data.csv as csv
use std.math.parse.data.xml as xml
use std.math.parse.data.sql as sql
