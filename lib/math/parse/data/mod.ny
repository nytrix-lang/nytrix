;; Keywords: data serialization zlib deflate compression csv delimited xml json yaml yml toml sql parse
;; Data-format facade for JSON, YAML, TOML, CSV, XML, SQL inspection, and zlib compression.
;; References:
;; - core.math.parse
module core.math.parse.data(json, yaml, toml, zlib, csv, xml, sql)
use core.math.parse.data.json as json
use core.math.parse.data.yaml as yaml
use core.math.parse.data.toml as toml
use core.math.parse.data.zlib as zlib
use core.math.parse.data.csv as csv
use core.math.parse.data.xml as xml
use core.math.parse.data.sql as sql
