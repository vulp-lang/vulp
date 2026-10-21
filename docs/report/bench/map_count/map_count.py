import sys

def main():
    data = sys.stdin.buffer.read().split()
    n = int(data[0])
    cnt = {}
    for s in data[1:n + 1]:
        x = int(s)
        cnt[x] = cnt.get(x, 0) + 1
    best = value = 0
    for k in sorted(cnt):
        if cnt[k] > best:
            best, value = cnt[k], k
    print(len(cnt), value, best)

main()
