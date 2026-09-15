/* Controlled string declarations for 4c; implementations come from libc. */
typedef unsigned long size_t;
size_t strlen(const char *text);
size_t strspn(const char *text, const char *accept);
void *memcpy(void *dest, const void *src, size_t count);
void *memset(void *dest, int value, size_t count);
char *strchr(const char *text, int c);
char *strpbrk(const char *text, const char *accept);
char *strstr(const char *text, const char *needle);
char *strerror(int error);
int strncmp(const char *left, const char *right, size_t count);
char *strcpy(char *dest, const char *src);
int strcmp(const char *left, const char *right);
