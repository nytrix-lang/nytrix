;; Exact crypt(3) compatibility vectors (not format-only checks).
use core.math.crypto.analysis.crypt as crypt
use core.math.crypto.analysis.dictionary (unix_crypt_hash)

fn test_unix_crypt_vectors() {
   assert(crypt.crypt_des("password", "ab") == "abJnggxhB/yWI", "des-password")
   assert(crypt.crypt_des("", "xx") == "xx95eIq3U1EUg", "des-empty")
   assert(crypt.crypt_md5("password", "$1$salt") == "$1$salt$qJH7.N4xYta3aEG/dfqo/0", "md5-password")
   assert(crypt.crypt_md5("", "$1$salt") == "$1$salt$UsdFqFVB.FsuinRDK5eE..", "md5-empty")
   assert(crypt.crypt_md5("password", "$1$1234567890") == "$1$12345678$o2n/JiO/h5VviOInWJ4OQ/", "md5-salt-trunc")
   assert(crypt.crypt_sha256("password", "$5$salt") == "$5$salt$Gcm6FsVtF/Qa77ZKD.iwsJlCVPY0XSMgLJL0Hnww/c1", "sha256-password")
   assert(crypt.crypt_sha256("", "$5$salt") == "$5$salt$HrcUzzoef72uxM/YhTU5BAi419Fblqlq//zyM.rIOG0", "sha256-empty")
   assert(crypt.crypt_sha512("password", "$6$salt") == "$6$salt$IxDD3jeSOb5eB1CX5LBsqZFVkJdido3OUILO5Ifz5iwMuTS4XMS130MTSuDDl3aCI6WouIL9AjRbLCelDCy.g.", "sha512-password")
   assert(crypt.crypt_sha512("", "$6$salt") == "$6$salt$r6qPcj2UeIkfklWHvleGJk8OKTInFYR/fxyuwcC656IWiZBpIFZ9.hMRG2ZQnnyMFrKOe461f9iT9Ljn0wJ5l.", "sha512-empty")
   assert(crypt.crypt_sha256("password", "$5$rounds=1000$salt") == "$5$rounds=1000$salt$p.wiWs2zrZ7irikO2AL64QDIJo00A3KDq2xWHpLJGgB", "sha256-rounds")
   assert(crypt.crypt_sha512("password", "$6$rounds=10000$salt") == "$6$rounds=10000$salt$dE5fLfpn2uXfkz.eouwYK/BjrHRu.piovQPjwlE06fDJHwMlg2l.IqEBUIfWBzf7YPXOAddB3FM7rnXHHKVNt.", "sha512-rounds")
   assert(crypt.crypt_sha512("password", "$6$longsalt0123456789") == "$6$longsalt01234567$7VT0MHBTsGu2OmxDDaLth3ZoH.4PEIpJ69CSAnswJoL4uiZBywVD7tSxI6ZBdAvUOQ.zwl6eXZFp1wnFlux5W/", "sha512-salt-trunc")
   assert(crypt.crypt_hash("password", "$7$salt") == "", "unknown scheme")
   assert(crypt.crypt_des("password", "?") == "", "bad DES salt")
   assert(unix_crypt_hash("password", "ab") == "abJnggxhB/yWI", "dictionary DES")
   assert(unix_crypt_hash("password", "$1$salt") == "$1$salt$qJH7.N4xYta3aEG/dfqo/0", "dictionary MD5")
}

#main {
   test_unix_crypt_vectors()
   print("crypt vectors ok")
}
