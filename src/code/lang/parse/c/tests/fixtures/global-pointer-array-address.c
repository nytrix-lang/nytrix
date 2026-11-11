/*
 * C fixture defining and indexing a global pointer-array table.
 */
struct nyc_bucket_item;
typedef struct nyc_bucket_item *nyc_bucket[8];
nyc_bucket nyc_bucket_table[4];
nyc_bucket *nyc_bucket_row(unsigned index) { return &nyc_bucket_table[index]; }
