#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

int is_safe(int64_t *board, int64_t row, int64_t col) {
    for (int64_t r = 0; r < row; r++) {
        int64_t c = board[r];
        if (c == col) return 0;
        int64_t dr = row - r;
        int64_t dc = col - c;
        if (dc < 0) dc = -dc;
        if (dc == dr) return 0;
    }
    return 1;
}

int64_t solve(int64_t *board, int64_t n, int64_t row) {
    if (row == n) return 1;
    int64_t count = 0;
    for (int64_t c = 0; c < n; c++) {
        if (is_safe(board, row, c)) {
            board[row] = c;
            count += solve(board, n, row + 1);
        }
    }
    return count;
}

int main() {
    int64_t n = 15;
    int64_t *board = (int64_t *)malloc(n * sizeof(int64_t));
    int64_t result = solve(board, n, 0);
    printf("n=%ld queens: %ld solutions\n", (long)n, (long)result);
    free(board);
    return 0;
}
