#include <stdio.h>

int main() {
    int letter = 65;
    while (letter <= 90) {
        putchar(letter);
        letter = letter + 1;
    }
    putchar(10);
    return 0;
}
