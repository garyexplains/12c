#ifndef TWELVE_C_STDARG_H
#define TWELVE_C_STDARG_H
/* The compiler represents va_list as a pointer to a private, 32-byte Linux
   AAPCS64 cursor. va_start and va_copy allocate independent cursor storage.
   The generated variadic prologue saves GP and FP registers separately. */
typedef struct __va_list_tag {
    void *stack;     /* next overflow stack argument in the caller's frame */
    void *gr_top;    /* top of saved GP regs (just past x7 slot) */
    void *vr_top;    /* top of saved FP/vector regs */
    int gr_offset;   /* -8 * remaining saved GP register slots */
    int vr_offset;
} __va_list_tag;

typedef __va_list_tag *va_list;
#endif