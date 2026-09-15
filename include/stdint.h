/* Controlled <stdint.h> for 4c. Both supported AArch64 targets use LP64, so
   long and unsigned long are 64 bits. The compiler does not support short,
   long long; 16-bit type support still needs correction. Integer constant
   macros use token pasting to select the appropriate literal type. */
typedef char int8_t;
typedef int int16_t;
typedef int int32_t;
typedef long int64_t;
typedef unsigned char uint8_t;
typedef unsigned int uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long uint64_t;
typedef long intptr_t;
typedef unsigned long uintptr_t;

#define INT8_MIN   (-128)
#define INT16_MIN  (-32768)
#define INT32_MIN  (-2147483647 - 1)
#define INT64_MIN  (-9223372036854775807L - 1)
#define INT8_MAX   127
#define INT16_MAX  32767
#define INT32_MAX  2147483647
#define INT64_MAX  9223372036854775807L
#define UINT8_MAX  255
#define UINT16_MAX 65535
#define UINT32_MAX 4294967295U
#define UINT64_MAX 18446744073709551615UL
#define INTPTR_MIN INT64_MIN
#define INTPTR_MAX INT64_MAX
#define UINTPTR_MAX UINT64_MAX

#define UINT64_C(value) value ## UL
#define INT64_C(value) value ## L
#define UINT32_C(value) value ## U
#define INT32_C(value) value
