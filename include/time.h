/* Controlled time declarations for 12c; implementations come from libc. */
typedef long clock_t;
#define CLOCKS_PER_SEC 1000000
clock_t clock(void);
