#include <stdio.h>

int print_message(char message[]) {
    int i = 0;
    while (message[i] != 0) {
        putchar(message[i]);
        i = i + 1;
    }
    return i;
}

int main() {
    char message[] = "Hello, arrays!\n";
    char *first = &message[0];
    *first = 'h';
    print_message(message);
    return 0;
}
