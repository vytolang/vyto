import sqlite3
import os

db_path = "benchmarks/logs.db"
if os.path.exists(db_path):
    os.remove(db_path)

conn = sqlite3.connect(db_path)
c = conn.cursor()
c.execute("CREATE TABLE logs (id INTEGER PRIMARY KEY, timestamp TEXT, level TEXT, service TEXT, message TEXT)")

with open("benchmarks/sample_large.log", "r") as f:
    total = 0
    for line in f:
        total += 1
        timestamp = line[:23]
        lb = line.find('[')
        rb = line.find(']')
        level = line[lb+1:rb] if lb >= 0 and rb >= 0 else ""
        lp = line.find('(')
        rp = line.find(')')
        service = line[lp+1:rp] if lp >= 0 and rp >= 0 else ""
        message = line[rp+2:].strip()
        c.execute("INSERT INTO logs (timestamp, level, service, message) VALUES (?, ?, ?, ?)",
                  (timestamp, level, service, message))
    conn.commit()
    print(f"wrote {total} rows")

# Read back
c.execute("SELECT id, timestamp, level, service, message FROM logs ORDER BY id")
read_count = sum(1 for _ in c)
print(f"read back {read_count} rows")

conn.close()
