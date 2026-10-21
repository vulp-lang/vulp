#include <bits/stdc++.h>
using namespace std;
int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    int t;
    cin >> t;
    while (t--) {
        int n;
        cin >> n;
        vector<long long> a(n);
        for (auto& x : a) cin >> x;
        long long best = a[0], cur = 0;
        for (long long x : a) {
            cur = max(cur + x, x);
            best = max(best, cur);
        }
        cout << best << "\n";
    }
}
