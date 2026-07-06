limit = 10000000
sieve = [True] * limit
sieve[0] = False
sieve[1] = False

p = 2
while p * p < limit:
    if sieve[p]:
        i = p * p
        while i < limit:
            sieve[i] = False
            i += p
    p += 1

count = sum(1 for x in sieve if x)
print(f"primes up to {limit}: {count}")
