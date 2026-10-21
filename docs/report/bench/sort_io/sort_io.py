import sys

def main():
    data = sys.stdin.buffer.read().split()
    n = int(data[0])
    a = sorted(map(int, data[1:n + 1]))
    sys.stdout.write(" ".join(map(str, a)) + "\n")

main()
