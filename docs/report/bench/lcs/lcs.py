import sys

def main():
    a, b = sys.stdin.buffer.read().split()
    n, m = len(a), len(b)
    prev = [0] * (m + 1)
    for i in range(1, n + 1):
        cur = [0] * (m + 1)
        ai = a[i - 1]
        for j in range(1, m + 1):
            if ai == b[j - 1]:
                cur[j] = prev[j - 1] + 1
            else:
                cur[j] = prev[j] if prev[j] > cur[j - 1] else cur[j - 1]
        prev = cur
    print(prev[m])

main()
