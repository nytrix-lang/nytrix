/*
 * Fixture: preserve wide integer enum constants through aliases.
 */
enum device_flags { DEVICE_FLAG = 1ULL << 34, DEVICE_LARGE_FLAG = 1ULL << 41 };
enum alias_flags { ALIAS_FLAG = DEVICE_FLAG, NEXT_FLAG };
enum high_flags { HIGH_FLAG = 1ULL << 63 };
enum high_alias { HIGH_ALIAS = HIGH_FLAG };

static unsigned long long read_flag(enum alias_flags value) { return value; }

int main(void) {
  __typeof__(ALIAS_FLAG) flag = ALIAS_FLAG;
  __typeof__(HIGH_ALIAS) high = HIGH_ALIAS;
  return sizeof(enum alias_flags) != 8 || sizeof(flag) != 8 ||
         sizeof(enum high_alias) != 8 || sizeof(high) != 8 ||
         read_flag(NEXT_FLAG) != (1ULL << 34) + 1 || flag != DEVICE_FLAG ||
         DEVICE_LARGE_FLAG != (1ULL << 41) || (high >> 63) != 1 || high < 0;
}
