import sys

def main():
    n = int(sys.stdin.buffer.readline())
    composite = bytearray(n + 1)
    count = last = 0
    for i in range(2, n + 1):
        if composite[i]:
            continue
        count += 1
        last = i
        if i * i <= n:
            composite[i * i::i] = b"\x01" * len(range(i * i, n + 1, i))
    print(count, last)

main()
