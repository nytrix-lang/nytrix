/* x86-64 retpoline thunk declarations. */
#ifndef NYR_X64_RETPOLINE_H
#define NYR_X64_RETPOLINE_H

/* Inline call thunk. The architectural target must already be in r11. */
static const unsigned char nyr_x64_inline_retpoline[] = {
    0xe8, 0x05, 0x00, 0x00, 0x00, /* call dispatch */
    0xe9, 0x14, 0x00, 0x00, 0x00, /* return: jmp after thunk */
    0xe8, 0x0a, 0x00, 0x00, 0x00, /* dispatch: call capture */
    0xf3, 0x90,                   /* speculation trap: pause */
    0x0f, 0xae, 0xe8,             /* lfence */
    0xe9, 0xf6, 0xff, 0xff, 0xff, /* jmp speculation trap */
    0x4c, 0x89, 0x1c, 0x24,       /* capture: mov r11,(rsp) */
    0xc3,                         /* ret to target */
    0x90};                        /* after-thunk landing */

#endif
