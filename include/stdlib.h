/* Controlled stdlib declarations for 12c; implementations come from libc. */
typedef unsigned long size_t;
#define NULL (void *)0
void *malloc(size_t size);
void free(void *pointer);
void *realloc(void *pointer, size_t size);
void exit(int status);
long strtol(const char *string, char **end, int base);
unsigned long strtoul(const char *string, char **end, int base);
unsigned long long strtoull(const char *string, char **end, int base);
#ifdef __linux__
double strtod(const char *string, char **end);
#endif
#define EXIT_FAILURE 1
#define EXIT_SUCCESS 0
