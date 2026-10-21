#include <bits/stdc++.h>
using namespace std;
int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    int n;
    cin >> n;
    map<long long, long long> cnt;
    for (int i = 0; i < n; i++) {
        long long x;
        cin >> x;
        cnt[x]++;
    }
    long long best = 0, value = 0;
    for (auto [k, c] : cnt)
        if (c > best) best = c, value = k;
    cout << cnt.size() << " " << value << " " << best << "\n";
}
