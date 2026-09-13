#include <stdio.h>

typedef int Number;
typedef Number *Number_Pointer;

Number total;
char letter = 'A';
Number_Pointer current;

void add(Number n) {
  *current = *current + n;
  return;
}

void print_letter(void) {
  putchar(letter);
}

int main(void) {
  current = &total;
  add(40);
  add(2);
  if (total != 42) return 1;
  print_letter();
  return 0;
}
