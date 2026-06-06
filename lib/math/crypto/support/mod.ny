;; Keywords: support utilities helpers bytes ascii printable rotation rol ror flags scanner line-scan math crypto
;; Shared crypto utilities for bytes, text, rotations, flag extraction, and line scanning.
;; References:
;; - std.math.crypto
module std.math.crypto.support(tools, scan_lines, collect_lines, bytes_contains, find_subseq, rol_bits, ror_bits, bytes_fixed_from_bigint, bytes_ascii, bytes_is_printable_ascii, bytes_has_prefix, extract_flag, extract_flag_bytes, list_uniq, max_bit_length, str_strip_ws, str_strip_bytes_literal)
