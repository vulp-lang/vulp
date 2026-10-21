#include <bits/stdc++.h>
using namespace std;
// The same recurrence as lcs_dp.vp, memoised by hand with a hash map.
string a, b;
unordered_map<long long, int> memo;
int lcs(int i, int j) {
    if (i == (int)a.size() || j == (int)b.size()) return 0;
    long long key = (long long)i << 32 | j;
    auto it = memo.find(key);
    if (it != memo.end()) return it->second;
    int r = a[i] == b[j] ? 1 + lcs(i + 1, j + 1) : max(lcs(i + 1, j), lcs(i, j + 1));
    memo[key] = r;
    return r;
}
int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    cin >> a >> b;
    cout << lcs(0, 0) << "\n";
}
