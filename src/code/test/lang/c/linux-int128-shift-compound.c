int main(void) {
  unsigned __int128 value = (unsigned __int128)0x8000000000000000ULL;
  value >>= 1;
  if ((unsigned long long)value != 0x4000000000000000ULL)
    return 1;
  value >>= 62;
  if ((unsigned long long)value != 1ULL)
    return 2;
  value = (unsigned __int128)0xfedcba9876543210ULL;
  value >>= 64;
  if ((unsigned long long)value != 0)
    return 3;
  value = (unsigned __int128)0xffffffffffffffffULL;
  value += 1;
  value >>= 64;
  if ((unsigned long long)value != 1ULL)
    return 4;
  return 0;
}
