/*
 * C front-end regression test: sysv byte array union argument.
 */
typedef unsigned char u8;

typedef union {
  u8 hsw[3];
  unsigned long xehp;
} intel_sseu_ss_mask_t;

unsigned short intel_slicemask_from_xehp_dssmask(intel_sseu_ss_mask_t dss_mask,
                                                 int dss_per_slice) {
  return dss_mask.hsw[0] + dss_per_slice;
}

unsigned long c_sseu_mask_argument(intel_sseu_ss_mask_t mask) {
  return intel_slicemask_from_xehp_dssmask(mask, 2);
}
