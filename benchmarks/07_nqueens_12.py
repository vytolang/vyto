def is_safe(board, row, col):
    for r in range(row):
        c = board[r]
        if c == col:
            return False
        dr = row - r
        dc = col - c
        if dc < 0:
            dc = -dc
        if dc == dr:
            return False
    return True

def solve(board, n, row):
    if row == n:
        return 1
    count = 0
    for c in range(n):
        if is_safe(board, row, c):
            board[row] = c
            count += solve(board, n, row + 1)
    return count

n = 12
board = [0] * n
result = solve(board, n, 0)
print(f"n={n} queens: {result} solutions")
