import sys
from heapq import heappush, heappop

def main():
    data = sys.stdin.buffer.read().split()
    n, m = int(data[0]), int(data[1])
    adj = [[] for _ in range(n + 1)]
    for e in range(m):
        u, v, w = int(data[2 + 3 * e]), int(data[3 + 3 * e]), int(data[4 + 3 * e])
        adj[u].append((v, w))
    INF = 1 << 62
    dist = [INF] * (n + 1)
    dist[1] = 0
    pq = [(0, 1)]
    while pq:
        d, u = heappop(pq)
        if d > dist[u]:
            continue
        for v, w in adj[u]:
            nd = d + w
            if nd < dist[v]:
                dist[v] = nd
                heappush(pq, (nd, v))
    reached = sum(1 for i in range(1, n + 1) if dist[i] < INF)
    total = sum(d for d in dist[1:] if d < INF)
    print(reached, total)

main()
