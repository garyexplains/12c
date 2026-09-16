/* Controlled stdio declarations for 12c; implementations come from libc. */
#include <stdarg.h>
typedef struct FILE FILE;
typedef unsigned long size_t;
extern FILE *stderr;
extern FILE *stdout;
#define EOF (-1)
#define SEEK_SET 0
FILE *fopen(const char *path, const char *mode);
FILE *tmpfile(void);
int fclose(FILE *stream);
int fflush(FILE *stream);
int ferror(FILE *stream);
int fseek(FILE *stream, long offset, int origin);
int fgetc(FILE *stream);
int fputc(int c, FILE *stream);
int fputs(const char *text, FILE *stream);
int puts(const char *text);
int snprintf(char *buffer, size_t size, const char *format, ...);
int sscanf(const char *text, const char *format, ...);
int vfprintf(FILE *stream, const char *format, va_list ap);
int putchar(int c);
int printf(const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);
