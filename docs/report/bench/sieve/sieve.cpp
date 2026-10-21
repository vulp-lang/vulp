#include <bits/stdc++.h>
using namespace std;
int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    long long n;
    cin >> n;
    vector<char> composite(n + 1);
    long long count = 0, last = 0;
    for (long long i = 2; i <= n; i++) {
        if (composite[i]) continue;
        count++;
        last = i;
        for (long long j = i * i; j <= n; j += i) composite[j] = 1;
    }
    cout << count << " " << last << "\n";
}
