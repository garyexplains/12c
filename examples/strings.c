#include <stdio.h>

int main() {
    char *message = "Hello, Jetson!";
    while (*message != 0) {
        putchar(*message);
        message = message + 1;
    }
    return 0;
}
