/* errno is thread-local storage reached through the platform's libc accessor. */
#ifdef __APPLE__
int *__error(void);
#define errno (*__error())
#else
int *__errno_location(void);
#define errno (*__errno_location())
#endif
