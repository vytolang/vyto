import sys

info = warn = error = debug = 0
auth = api = db = cache = scheduler = 0
total = 0

with open("benchmarks/sample_large.log", "r") as f:
    for line in f:
        total += 1
        if "[INFO]" in line: info += 1
        if "[WARN]" in line: warn += 1
        if "[ERROR]" in line: error += 1
        if "[DEBUG]" in line: debug += 1
        if "(auth)" in line: auth += 1
        if "(api)" in line: api += 1
        if "(db)" in line: db += 1
        if "(cache)" in line: cache += 1
        if "(scheduler)" in line: scheduler += 1

print(f"total lines: {total}")
print(f"INFO: {info}, WARN: {warn}, ERROR: {error}, DEBUG: {debug}")
print(f"auth: {auth}, api: {api}, db: {db}, cache: {cache}, scheduler: {scheduler}")
