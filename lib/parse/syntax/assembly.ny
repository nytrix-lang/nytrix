;; Keywords: syntax assembly asm
;; Assembly syntax highlighter
module std.parse.syntax.assembly(tokenize)
use std.core
use std.core.str as str
use std.parse.syntax.helpers as _h

def DIRECTIVES = ".align;.ascii;.asciz;.balign;.byte;.code16;.code32;.code64;.comm;.data;.def;.double;.else;.elseif;.end;.endif;.endm;.endr;.equ;.equiv;.err;.extern;.file;.fill;.float;.globl;.global;.hidden;.ident;.if;.ifdef;.ifndef;.include;.int;.irp;.irpc;.lcomm;.line;.linkonce;.list;.long;.macro;.noaltmacro;.nolist;.octa;.org;.p2align;.popsection;.previous;.pushsection;.quad;.rept;.rodata;.section;.set;.short;.size;.skip;.space;.string;.text;.type;.uleb128;.weak;.word;align;bits;cpu;default;equ;extern;global;section;segment;struc;endstruc;absolute;common;org;times;resb;resw;resd;resq;db;dw;dd;dq;dt;do;dy;dz;incbin"
def OPCODES = "aaa;aad;aam;aas;adc;adcx;add;addpd;addps;addsd;addss;adox;and;andn;andnpd;andnps;andpd;andps;arpl;beq;bge;bgt;bhi;bhs;ble;blo;bls;blt;bmi;bne;bpl;bvc;bvs;bl;bx;b;cqo;call;cbw;cdq;clc;cld;cli;clts;cmc;cmova;cmovae;cmovb;cmovbe;cmovc;cmove;cmovg;cmovge;cmovl;cmovle;cmovna;cmovnae;cmovnb;cmovnbe;cmovnc;cmovne;cmovng;cmovnge;cmovnl;cmovnle;cmovno;cmovnp;cmovns;cmovnz;cmovo;cmovp;cmovpe;cmovpo;cmovs;cmovz;cmp;cmppd;cmpps;cmpsb;cmpsd;cmpsq;cmpss;cmpsw;cmpxchg;cmpxchg16b;cmpxchg8b;cpuid;cpsid;cpsie;cvtpd2pi;cvtpd2ps;cvtpi2pd;cvtpi2ps;cvtps2pd;cvtps2pi;cvtsd2si;cvtsd2ss;cvtsi2sd;cvtsi2ss;cvtss2sd;cvtss2si;cvttpd2pi;cvttpd2dq;cvttps2dq;cvttsd2si;cvttss2si;cwd;daa;das;dec;div;divpd;divps;divsd;divss;emms;enter;f2xm1;fabs;fadd;faddp;fbld;fbstp;fchs;fclex;fcmovb;fcmove;fcmovbe;fcmovu;fcmovnb;fcmovne;fcmovnbe;fcmovnu;fcom;fcomi;fcomip;fcomp;fcompp;fcos;fdecstp;fdiv;fdivp;fdivr;fdivrp;ffree;fiadd;ficom;ficomp;fidiv;fidivr;fild;fimul;fincstp;finit;fist;fistp;fisub;fisubr;fld;fld1;fldcw;fldenv;fldl2e;fldl2t;fldlg2;fldln2;fldpi;fldz;fmul;fmulp;fnclex;fninit;fnop;fnsave;fnstcw;fnstenv;fnstsw;fpatan;fprem;fprem1;fptan;frndint;frstor;fsave;fscale;fsin;fsincos;fsqrt;fst;fstcw;fstenv;fstp;fstsw;fsub;fsubp;fsubr;fsubrp;ftst;fucom;fucomi;fucomip;fucomp;fucompp;fwait;fxam;fxch;fxrstor;fxsave;fxtract;fyl2x;fyl2xp1;hlt;idiv;imul;in;inc;insb;insd;insw;int;into;invd;invlpg;iret;iretd;iretq;ja;jae;jb;jbe;jc;jcxz;je;jecxz;jg;jge;jl;jle;jmp;jna;jnae;jnb;jnbe;jnc;jne;jng;jnge;jnl;jnle;jno;jnp;jns;jnz;jo;jp;jpe;jpo;jrcxz;js;jz;lahf;lar;ldmxcsr;lds;lea;leave;les;lfence;lfs;lgdt;lgs;lidt;lldt;lmsw;lock;lodsb;lodsd;lodsq;lodsw;loop;loope;loopne;loopnz;loopz;lsl;lss;ltr;maskmovdqu;maskmovq;maxpd;maxps;maxsd;maxss;mfence;minpd;minps;minsd;minss;mov;movabs;movapd;movaps;movbe;movd;movddup;movdq2q;movdqa;movdqu;movhlps;movhpd;movhps;movlhps;movlpd;movlps;movmskpd;movmskps;movntdq;movnti;movntpd;movntps;movntq;movq;movq2dq;movsb;movsd;movshdup;movsldup;movsq;movss;movsw;movsx;movsxd;movupd;movups;movzx;mul;mulpd;mulps;mulsd;mulss;neg;nop;not;or;orpd;orps;out;outsb;outsd;outsw;pause;pavgb;pavgw;pextrw;pinsrw;pmaddwd;pmaxsw;pmaxub;pminsw;pminub;pmovmskb;pmulhuw;pmulhw;pmullw;pop;popa;popad;popcnt;popf;popfd;popfq;prefetchnta;prefetcht0;prefetcht1;prefetcht2;psadbw;pshufd;pshufhw;pshuflw;pshufw;pslldq;psllq;psllw;psrad;psraw;psrldq;psrlq;psrlw;psubb;psubd;psubq;psubsb;psubsw;psubusb;psubusw;psubw;punpckhbw;punpckhdq;punpckhqdq;punpckhwd;punpcklbw;punpckldq;punpcklqdq;punpcklwd;push;pusha;pushad;pushf;pushfd;pushfq;pxor;rcl;rcr;rdmsr;rdpmc;rdtsc;ret;retf;rol;ror;rsm;rsqrtps;rsqrtss;sahf;sal;sar;sbb;scasb;scasd;scasq;scasw;sfence;sgdt;shl;shld;shr;shrd;sidt;sldt;smsw;sqrtpd;sqrtps;sqrtsd;sqrtss;stc;std;sti;stmxcsr;stosb;stosd;stosq;stosw;str;sub;subpd;subps;subsd;subss;syscall;sysenter;sysexit;sysret;test;ucomisd;ucomiss;ud2;unpckhpd;unpckhps;unpcklpd;unpcklps;verr;verw;wait;wbinvd;wrmsr;xadd;xchg;xlat;xlatb;xor;xorpd;xorps;ldr;ldrb;ldrh;ldrsb;ldrsh;strb;strh;stm;ldm;push;pop;adr;adrp;movk;movn;movz;mvn;cmp;cmn;tst;teq;orr;eor;bic;lsl;lsr;asr;ror;mul;mla;mls;smull;umull;sdiv;udiv;svc;sys;nop;wfe;wfi;sev;yield;fmov;fcmp;fadd;fsub;fmul;fdiv;fsqrt"
def REGISTERS = "al;ah;ax;eax;rax;bl;bh;bx;ebx;rbx;cl;ch;cx;ecx;rcx;dl;dh;dx;edx;rdx;sil;si;esi;rsi;dil;di;edi;rdi;bpl;bp;ebp;rbp;spl;sp;esp;rsp;rip;eip;cs;ds;es;fs;gs;ss;cr0;cr2;cr3;cr4;cr8;dr0;dr1;dr2;dr3;dr6;dr7;r0;r1;r2;r3;r4;r5;r6;r7;r8;r9;r10;r11;r12;r13;r14;r15;r8b;r9b;r10b;r11b;r12b;r13b;r14b;r15b;r8w;r9w;r10w;r11w;r12w;r13w;r14w;r15w;r8d;r9d;r10d;r11d;r12d;r13d;r14d;r15d;xmm0;xmm1;xmm2;xmm3;xmm4;xmm5;xmm6;xmm7;xmm8;xmm9;xmm10;xmm11;xmm12;xmm13;xmm14;xmm15;ymm0;ymm1;ymm2;ymm3;ymm4;ymm5;ymm6;ymm7;ymm8;ymm9;ymm10;ymm11;ymm12;ymm13;ymm14;ymm15;zmm0;zmm1;zmm2;zmm3;zmm4;zmm5;zmm6;zmm7;zmm8;zmm9;zmm10;zmm11;zmm12;zmm13;zmm14;zmm15;w0;w1;w2;w3;w4;w5;w6;w7;w8;w9;w10;w11;w12;w13;w14;w15;w16;w17;w18;w19;w20;w21;w22;w23;w24;w25;w26;w27;w28;w29;w30;x0;x1;x2;x3;x4;x5;x6;x7;x8;x9;x10;x11;x12;x13;x14;x15;x16;x17;x18;x19;x20;x21;x22;x23;x24;x25;x26;x27;x28;x29;x30;sp;lr;pc;fp;zr;wzr;xzr;v0;v1;v2;v3;v4;v5;v6;v7;v8;v9;v10;v11;v12;v13;v14;v15;v16;v17;v18;v19;v20;v21;v22;v23;v24;v25;v26;v27;v28;v29;v30;v31;q0;q1;q2;q3;q4;q5;q6;q7;q8;q9;q10;q11;q12;q13;q14;q15;q16;q17;q18;q19;q20;q21;q22;q23;q24;q25;q26;q27;q28;q29;q30;q31;d0;d1;d2;d3;d4;d5;d6;d7;d8;d9;d10;d11;d12;d13;d14;d15;d16;d17;d18;d19;d20;d21;d22;d23;d24;d25;d26;d27;d28;d29;d30;d31;s0;s1;s2;s3;s4;s5;s6;s7;s8;s9;s10;s11;s12;s13;s14;s15;s16;s17;s18;s19;s20;s21;s22;s23;s24;s25;s26;s27;s28;s29;s30;s31"
def SIZE_WORDS = "byte;word;dword;qword;tword;oword;yword;zword;ptr;near;far;short;rel;offset;flat"

fn _is_digit(int: b): bool { b >= 48 && b <= 57 }

fn _is_hex_digit(int: b): bool { _is_digit(b) || (b >= 65 && b <= 70) || (b >= 97 && b <= 102) }

fn _is_ident_start(int: b): bool { _h.is_alpha_ch(b) || b == 46 || b == 64 || b == 36 || b == 95 }

fn _is_ident_part(int: b): bool { _h.is_alnum_ch(b) || b == 46 || b == 64 || b == 36 || b == 95 || b == 63 }

fn _is_reg_prefix(int: b): bool { b == 37 || b == 36 }

fn _scan_number(str: source, int: i, int: src_len): int {
   mut j = i
   if(j < src_len && (load8(source, j) == 45 || load8(source, j) == 43)){ j += 1 }
   if(j + 1 < src_len && load8(source, j) == 48 && (load8(source, j + 1) == 120 || load8(source, j + 1) == 88)){
      j += 2
      while(j < src_len && (_is_hex_digit(load8(source, j)) || load8(source, j) == 95)){ j += 1 }
   } else {
      while(j < src_len){
         def c = load8(source, j)
         if(_is_hex_digit(c) || c == 46 || c == 95 || c == 120 || c == 88 || c == 98 || c == 66 || c == 111 || c == 79 || c == 104 || c == 72){ j += 1 }
         else { break }
      }
   }
   j
}

fn _scan_ident(str: source, int: i, int: src_len): int {
   mut j = i
   while(j < src_len && _is_ident_part(load8(source, j))){ j += 1 }
   j
}

fn tokenize(str: source, list: out_tokens): list {
   def src_len = source.len
   mut i = 0
   while(i < src_len){
      def idx = i + 0
      i = idx
      def ch = load8(source, i)
      if(ch == 32 || ch == 9 || ch == 10 || ch == 13){
         mut j = i
         while(j < src_len){ def c = load8(source, j) if(c != 32 && c != 9 && c != 10 && c != 13){ break } j += 1 }
         out_tokens = _h.add_tok(out_tokens, 14, i, j - i)
         i = j
      } elif(ch == 47 && i + 1 < src_len && load8(source, i + 1) == 47){
         def j = _h.scan_line(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 4, i, j - i)
         i = j
      } elif(ch == 47 && i + 1 < src_len && load8(source, i + 1) == 42){
         mut j = i + 2
         while(j + 1 < src_len){ if(load8(source, j) == 42 && load8(source, j + 1) == 47){ j += 2 break } j += 1 }
         out_tokens = _h.add_tok(out_tokens, 4, i, j - i)
         i = j
      } elif(ch == 59 || ch == 33){
         def j = _h.scan_line(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 4, i, j - i)
         i = j
      } elif(ch == 35 && !(i + 1 < src_len && (_is_digit(load8(source, i + 1)) || load8(source, i + 1) == 43 || load8(source, i + 1) == 45))){
         def j = _h.scan_line(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 4, i, j - i)
         i = j
      } elif(ch == 34 || ch == 39 || ch == 96){
         def j = _h.scan_quoted(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 2, i, j - i)
         i = j
      } elif(_is_digit(ch)){
         def j = _scan_number(source, i, src_len)
         if(j < src_len && load8(source, j) == 58){
            out_tokens = _h.add_tok(out_tokens, 0, i, j - i)
            out_tokens = _h.add_tok(out_tokens, 7, j, 1)
            i = j + 1
         } else {
            out_tokens = _h.add_tok(out_tokens, 3, i, j - i)
            i = j
         }
      } elif((ch == 45 || ch == 43) && i + 1 < src_len && _is_digit(load8(source, i + 1))){
         def j = _scan_number(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 3, i, j - i)
         i = j
      } elif(_is_reg_prefix(ch) && i + 1 < src_len && _is_ident_start(load8(source, i + 1))){
         def j = _scan_ident(source, i + 1, src_len)
         def word = str.str_slice(source, i + 1, j)
         if(_h.in_list_ci(word, REGISTERS)){ out_tokens = _h.add_tok(out_tokens, 1, i, j - i) }
         else { out_tokens = _h.add_tok(out_tokens, 6, i, 1) out_tokens = _h.add_tok(out_tokens, 8, i + 1, j - i - 1) }
         i = j
      } elif(_is_ident_start(ch)){
         def j = _scan_ident(source, i, src_len)
         def word = str.str_slice(source, i, j)
         if(j < src_len && load8(source, j) == 58){
            out_tokens = _h.add_tok(out_tokens, 0, i, j - i)
            out_tokens = _h.add_tok(out_tokens, 7, j, 1)
            i = j + 1
         } elif(_h.in_list_ci(word, DIRECTIVES)){
            out_tokens = _h.add_tok(out_tokens, 10, i, j - i)
            i = j
         } elif(_h.in_list_ci(word, REGISTERS)){
            out_tokens = _h.add_tok(out_tokens, 1, i, j - i)
            i = j
         } elif(_h.in_list_ci(word, SIZE_WORDS)){
            out_tokens = _h.add_tok(out_tokens, 1, i, j - i)
            i = j
         } elif(_h.in_list_ci(word, OPCODES)){
            out_tokens = _h.add_tok(out_tokens, 5, i, j - i)
            i = j
         } else {
            out_tokens = _h.add_tok(out_tokens, 8, i, j - i)
            i = j
         }
      } elif(ch == 43 || ch == 45 || ch == 42 || ch == 47 || ch == 37 || ch == 61 || ch == 60 || ch == 62 || ch == 38 || ch == 124 || ch == 94 || ch == 126 || ch == 35){
         mut j = i
         while(j < src_len){
            def c = load8(source, j)
            if(c == 43 || c == 45 || c == 42 || c == 47 || c == 37 || c == 61 || c == 60 || c == 62 || c == 38 || c == 124 || c == 94 || c == 126 || c == 35){ j += 1 }
            else { break }
         }
         out_tokens = _h.add_tok(out_tokens, 6, i, j - i)
         i = j
      } elif(ch == 40 || ch == 41 || ch == 91 || ch == 93 || ch == 123 || ch == 125 || ch == 44 || ch == 58){
         out_tokens = _h.add_tok(out_tokens, 7, i, 1)
         i += 1
      } else {
         out_tokens = _h.add_tok(out_tokens, 14, i, 1)
         i += 1
      }
   }
   out_tokens
}
