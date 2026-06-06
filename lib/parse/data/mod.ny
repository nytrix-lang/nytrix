;; Keywords: data serialization zlib deflate compression csv delimited xml json yaml yml toml sql parse
;; Data-format facade for JSON, YAML, TOML, CSV, XML, SQL inspection, and zlib compression.
;; References:
;; - std.parse
module std.parse.data(json, yaml, toml, zlib, csv, xml, sql)
use std.parse.data.json as json
use std.parse.data.yaml as yaml
use std.parse.data.toml as toml
use std.parse.data.zlib as zlib
use std.parse.data.csv as csv
use std.parse.data.xml as xml
use std.parse.data.sql as sql
