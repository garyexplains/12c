/* Controlled stdlib declarations for 4c; implementations come from libc. */
typedef unsigned long size_t;
void *malloc(size_t size);
void free(void *pointer);
long strtol(const char *string, char **end, int base);
#define EXIT_FAILURE 1
#define EXIT_SUCCESS 0
