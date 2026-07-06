double mx_clamp(double x, double lo, double hi) {
    return x < lo ? lo : x > hi ? hi : x;
}

long long mx_fib(int n) {
    long long a = 0, b = 1;
    while (n-- > 0) {
        long long t = a + b;
        a = b;
        b = t;
    }
    return a;
}
