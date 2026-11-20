struct record {
  unsigned long key;
  unsigned long value;
};

#define IN_REGION                                                              \
  __attribute__((section("ny_"                                                 \
                         "records"),                                           \
                 aligned(64)))
extern IN_REGION __typeof__(struct record) per;
IN_REGION __typeof__(struct record) per = {11, 29};
IN_REGION __typeof__(unsigned long) zero_on_start;

struct record trailing
    __attribute__((section("ny_records"), aligned(128))) = {13, 31};
extern struct record inherited __attribute__((aligned(256)));
struct record inherited __attribute__((section("ny_records"))) = {17, 37};

extern unsigned char __start_ny_records[], __stop_ny_records[];

int main(void) {
  unsigned long start = (unsigned long)__start_ny_records;
  unsigned long stop = (unsigned long)__stop_ny_records;
  unsigned long record = (unsigned long)&per;
  unsigned long zero = (unsigned long)&zero_on_start;
  if (record < start || record + sizeof(per) > stop || (record & 63))
    return 1;
  if (zero < start || zero + sizeof(zero_on_start) > stop || (zero & 63))
    return 2;
  if (((unsigned long)&trailing & 127) || ((unsigned long)&inherited & 255))
    return 3;
  return per.key != 11 || per.value != 29 || zero_on_start != 0 ||
         trailing.key != 13 || inherited.value != 37;
}
