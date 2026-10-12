#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int n = 5;
    int *arr = malloc(n * sizeof(int));
    if (!arr) { printf("alloc failed\n"); return 1; }

    for (int i = 0; i < n; i++) arr[i] = i * 10;
    for (int i = 0; i < n; i++) printf("arr[%d] = %d\n", i, arr[i]);

    free(arr);
    return 0;
}