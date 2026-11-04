struct nyc_bucket_item;
typedef struct nyc_bucket_item *nyc_bucket[8];
extern nyc_bucket nyc_bucket_table[4];
extern nyc_bucket *nyc_bucket_row(unsigned);
int main(void) {
  return nyc_bucket_row(2) != &nyc_bucket_table[2];
}
