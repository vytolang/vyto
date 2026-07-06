def fib(n):
    if n <= 1:
        return n
    return fib(n - 1) + fib(n - 2)

n = 40
result = fib(n)
print(f"fib({n}) = {result}")
