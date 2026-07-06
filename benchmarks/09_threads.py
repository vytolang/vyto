import concurrent.futures
import multiprocessing

def fill_array(size):
    return [(i * 7) % 1000 for i in range(size)]

def sum_chunk(args):
    data, start, count = args
    return sum(data[start:start + count])

def parallel_sum(data, size, num_workers):
    chunk = size // num_workers
    chunks = []
    for t in range(num_workers):
        start = t * chunk
        count = chunk if t < num_workers - 1 else size - t * chunk
        chunks.append((data, start, count))

    with concurrent.futures.ProcessPoolExecutor(max_workers=num_workers) as executor:
        results = executor.map(sum_chunk, chunks)
        return sum(results)

def main():
    num_workers = multiprocessing.cpu_count()
    size = 100000000

    data = fill_array(size)

    # Single-threaded
    sum1 = sum(data)
    print(f"single-threaded sum = {sum1}")

    # Multi-process
    sum2 = parallel_sum(data, size, num_workers)
    print(f"multi-process sum ({num_workers} workers) = {sum2}")

if __name__ == "__main__":
    main()
