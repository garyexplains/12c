/* Controlled stdio declarations for 4c; implementations come from libc. */
typedef struct FILE FILE;
extern FILE *stderr;
int putchar(int c);
int printf(const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);
