#include <stdio.h>
#include <unistd.h>

int main() {
  int i = 0;
  while (1) {
    printf("Loop %d\n", i++);
    sleep(4);
  }
}
