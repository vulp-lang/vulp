import sys

def main():
    data = sys.stdin.buffer.read().split()
    t = int(data[0])
    pos = 1
    out = []
    for _ in range(t):
        n = int(data[pos])
        a = data[pos + 1:pos + 1 + n]
        pos += 1 + n
        best = cur = None
        for s in a:
            x = int(s)
            cur = x if cur is None else max(cur + x, x)
            best = cur if best is None else max(best, cur)
        out.append(best)
    sys.stdout.write("\n".join(map(str, out)) + "\n")

main()
