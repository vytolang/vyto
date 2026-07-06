size = 300
a = [[float(i + j) for j in range(size)] for i in range(size)]
b = [[float(i - j) for j in range(size)] for i in range(size)]
c = [[0.0] * size for _ in range(size)]

for i in range(size):
    for j in range(size):
        s = 0.0
        for k in range(size):
            s += a[i][k] * b[k][j]
        c[i][j] = s

print(f"matrix {size}x{size} multiplied")
print(f"c[0][0] = {c[0][0]}")
