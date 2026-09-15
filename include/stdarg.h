#ifndef FOUR_C_STDARG_H
#define FOUR_C_STDARG_H
/* Controlled <stdarg.h> for 4c. The va_list type follows the AArch64 AAPCS:
   a pointer to __va_list_tag. Variadic function definitions save x0..x7 to
   a known offset in their frame; va_start initializes a __va_list_tag in the
   frame so libc's vfprintf can read unnamed arguments. */
typedef struct __va_list_tag {
    void *stack;     /* next overflow stack argument in the caller's frame */
    void *gr_top;    /* top of saved GP regs (just past x7 slot) */
    void *vr_top;    /* top of saved FP/vector regs */
    int gr_offset;   /* -N-1 where N is named GP regs; consumed by vfprintf */
    int vr_offset;
} __va_list_tag;

typedef __va_list_tag *va_list;
#endif