#include <stdio.h>

int sum(int n) {
    if (n <= 0) {
        return 0;
    }
    return n + sum(n - 1);
}

int main() {
    putchar(sum(10) + 10);
    return 0;
}
