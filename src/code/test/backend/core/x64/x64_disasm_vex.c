/*
 * Keep the internal x86-64 decoder inverse to the VEX move forms emitted by
 * the vector encoder, including high vector-register extensions.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern const char *nyr_internal_x64_disasm(const unsigned char *bytes,
                                           int64_t length, int64_t address);

int main(void) {
  static const unsigned char encoded[] = {
      0xc4, 0xe1, 0x7e, 0x6f, 0x85, 0,    0, 0, 0, /* vmovdqu ymm0, [rbp] */
      0xc4, 0x61, 0x7e, 0x7f, 0x85, 0,    0, 0, 0, /* vmovdqu [rbp], ymm8 */
      0xc4, 0x61, 0x7d, 0x6f, 0xc1,                /* vmovdqa ymm8, ymm1 */
      0xc4, 0xe1, 0x7d, 0xd4, 0xc1,                /* vpaddq ymm0, ymm0, ymm1 */
      0xc4, 0xe2, 0x7d, 0x19, 0x85, 0,    0, 0, 0, /* vbroadcastsd ymm0,[rbp] */
      0xc4, 0xe2, 0xf5, 0xa8, 0xc2,       /* vfmadd213pd ymm0,ymm1,ymm2 */
      0xc4, 0xe3, 0x7d, 0x19, 0xc1, 0x01, /* vextractf128 xmm1,ymm0,1 */
      0xc4, 0xe1, 0x79, 0xd4, 0xc1,       /* vpaddq xmm0, xmm0, xmm1 */
      0xc4, 0xe2, 0xf1, 0xa8, 0xc2,       /* vfmadd213pd xmm0,xmm1,xmm2 */
      0xc4, 0xe1, 0x75, 0x58, 0xc2,       /* vaddpd ymm0,ymm1,ymm2 */
      0xc4, 0xe1, 0x72, 0x58, 0xc2,       /* vaddss xmm0,xmm1,xmm2 */
      0xc4, 0xe2, 0xfd, 0x59, 0x85, 0,    0, 0, 0, /* vpbroadcastq */
      0xc4, 0xe2, 0x7d, 0x19, 0xc1,       /* vbroadcastsd ymm0, xmm1 */
      0xc4, 0xe2, 0xfd, 0x59, 0xc1,       /* vpbroadcastq ymm0, xmm1 */
      0xc5, 0xf9, 0x2e, 0xc1,             /* vucomisd xmm0, xmm1 */
      0xc5, 0xf8, 0x2e, 0xc1,             /* vucomiss xmm0, xmm1 */
      0xc5, 0xf8, 0x77,                   /* vzeroupper */
      0xc5, 0xfc, 0x77,                   /* vzeroall */
      0xc4, 0xc1, 0x0b, 0x51, 0xf6,       /* vsqrtsd xmm6,xmm14,xmm14 */
      0xc5, 0xf9, 0x70, 0xc0, 0x00,       /* vpshufd xmm0,xmm0,0 */
      0xc4, 0xe3, 0x7d, 0x18, 0xc0, 0x01, /* vinsertf128 ymm0,ymm0,xmm0,1 */
      0xc4, 0xe1, 0x75, 0x54, 0xc2,       /* vandpd ymm0,ymm1,ymm2 */
      0xc4, 0xe1, 0x75, 0x55, 0xc2,       /* vandnpd ymm0,ymm1,ymm2 */
      0xc4, 0xe1, 0x75, 0x56, 0xc2,       /* vorpd ymm0,ymm1,ymm2 */
      0xc4, 0xe1, 0x75, 0x57, 0xc2,       /* vxorpd ymm0,ymm1,ymm2 */
      0xc4, 0xe1, 0x7d, 0x73, 0xd5, 0x3f, /* vpsrlq ymm5,ymm5,63 */
      0xc4, 0xe1, 0x7d, 0x72, 0xe2, 0x01, /* vpsrad ymm2,ymm2,1 */
      0xc4, 0xe1, 0x05, 0x76, 0xff,       /* vpcmpeqd ymm7,ymm15,ymm7 */
  };
  const char *listing =
      nyr_internal_x64_disasm(encoded, (int64_t)sizeof(encoded), 0x1000);
  if (!strstr(listing, "4096\tvmovdqu\tymm0, ymmword ptr [rbp+0]\t9\t") ||
      !strstr(listing, "4105\tvmovdqu\tymmword ptr [rbp+0], ymm8\t9\t") ||
      !strstr(listing, "4114\tvmovdqa\tymm8, ymm1\t5\t") ||
      !strstr(listing, "4119\tvpaddq\tymm0, ymm0, ymm1\t5\t") ||
      !strstr(listing, "4124\tvbroadcastsd\tymm0, qword ptr [rbp+0]\t9\t") ||
      !strstr(listing, "4133\tvfmadd213pd\tymm0, ymm1, ymm2\t5\t") ||
      !strstr(listing, "4138\tvextractf128\txmm1, ymm0, 1\t6\t") ||
      !strstr(listing, "4144\tvpaddq\txmm0, xmm0, xmm1\t5\t") ||
      !strstr(listing, "4149\tvfmadd213pd\txmm0, xmm1, xmm2\t5\t") ||
      !strstr(listing, "4154\tvaddpd\tymm0, ymm1, ymm2\t5\t") ||
      !strstr(listing, "4159\tvaddss\txmm0, xmm1, xmm2\t5\t") ||
      !strstr(listing, "4164\tvpbroadcastq\tymm0, qword ptr [rbp+0]\t9\t") ||
      !strstr(listing, "4173\tvbroadcastsd\tymm0, xmm1\t5\t") ||
      !strstr(listing, "4178\tvpbroadcastq\tymm0, xmm1\t5\t") ||
      !strstr(listing, "4183\tvucomisd\txmm0, xmm1\t4\t") ||
      !strstr(listing, "4187\tvucomiss\txmm0, xmm1\t4\t") ||
      !strstr(listing, "4191\tvzeroupper\t\t3\t") ||
      !strstr(listing, "4194\tvzeroall\t\t3\t") ||
      !strstr(listing, "4197\tvsqrtsd\txmm6, xmm14, xmm14\t5\t") ||
      !strstr(listing, "4202\tvpshufd\txmm0, xmm0, 0x0\t5\t") ||
      !strstr(listing, "4207\tvinsertf128\tymm0, ymm0, xmm0, 1\t6\t") ||
      !strstr(listing, "4213\tvandpd\tymm0, ymm1, ymm2\t5\t") ||
      !strstr(listing, "4218\tvandnpd\tymm0, ymm1, ymm2\t5\t") ||
      !strstr(listing, "4223\tvorpd\tymm0, ymm1, ymm2\t5\t") ||
      !strstr(listing, "4228\tvxorpd\tymm0, ymm1, ymm2\t5\t") ||
      !strstr(listing, "4233\tvpsrlq\tymm5, 0x3f\t6\t") ||
      !strstr(listing, "4239\tvpsrad\tymm2, 0x1\t6\t") ||
      !strstr(listing, "4245\tvpcmpeqd\tymm7, ymm15, ymm7\t5\t") ||
      strstr(listing, "\tunsupported\t")) {
    fprintf(stderr, "[x64-disasm-vex] encoded VEX listing mismatch:\n%s",
            listing);
    return 1;
  }
  static const unsigned char scalar_shuffle_forms[] = {
      0xc4, 0x41, 0x00, 0x12, 0xfe, /* vmovhlps xmm15,xmm0,xmm14 */
      0xc5, 0xf8, 0x15, 0xc0,       /* vunpckhps xmm0,xmm0,xmm0 */
      0xc5, 0xf9, 0x15, 0xc0,       /* vunpckhpd xmm0,xmm0,xmm0 */
      0xc5, 0xfa, 0x12, 0xda,       /* vmovsldup xmm3,xmm2 */
      0xc5, 0xfb, 0x12, 0xda,       /* vmovddup xmm3,xmm2 */
      0xc5, 0xf9, 0xc6, 0xc3, 0x02, /* vshufpd xmm0,xmm0,xmm3,2 */
  };
  listing = nyr_internal_x64_disasm(scalar_shuffle_forms,
                                    (int64_t)sizeof(scalar_shuffle_forms),
                                    0x1700);
  if (!strstr(listing, "vmovhlps\t") ||
      !strstr(listing, "vunpckhps\t") ||
      !strstr(listing, "vunpckhpd\t") ||
      !strstr(listing, "vmovsldup\t") ||
      !strstr(listing, "vmovddup\t") ||
      !strstr(listing, "vshufpd\txmm0, xmm0, xmm3, 0x2") ||
      strstr(listing, "\tunsupported\t")) {
    fprintf(stderr, "[x64-disasm-vex] packed shuffle decode mismatch:\n%s",
            listing);
    return 1;
  }
  static const unsigned char unsupported[] = {0xc4, 0xe1, 0x7d, 0x77, 0xc0};
  listing = nyr_internal_x64_disasm(unsupported, (int64_t)sizeof(unsupported),
                                    0x2000);
  if (!strstr(listing, "\tunsupported\t")) {
    fprintf(stderr,
            "[x64-disasm-vex] unsupported VEX encoding was not rejected:\n%s",
            listing);
    return 1;
  }
  static const unsigned char vex2_forms[] = {
      0xc5, 0xfd, 0x10, 0x45, 0xf8, /* vmovupd ymm0, [rbp-8] */
      0xc5, 0xfd, 0x11, 0x45, 0xf8, /* vmovupd [rbp-8], ymm0 */
      0xc5, 0xf9, 0x28, 0xc1,       /* vmovapd xmm0, xmm1 */
      0xc5, 0xf5, 0x58, 0xc2,       /* vaddpd ymm0, ymm1, ymm2 */
  };
  listing = nyr_internal_x64_disasm(vex2_forms, sizeof(vex2_forms), 0x1800);
  if (!strstr(listing, "6144\tvmovupd\tymm0, ymmword ptr [rbp-8]\t5\t") ||
      !strstr(listing, "6149\tvmovupd\tymmword ptr [rbp-8], ymm0\t5\t") ||
      !strstr(listing, "6154\tvmovapd\txmm0, xmm1\t4\t") ||
      !strstr(listing, "6158\tvaddpd\tymm0, ymm1, ymm2\t4\t") ||
      strstr(listing, "\tunsupported\t")) {
    fprintf(stderr, "[x64-disasm-vex] two-byte VEX decode mismatch:\n%s",
            listing);
    return 1;
  }
  puts("[x64-disasm-vex] emitted VEX forms decode to hand-checked listings");
  return 0;
}
