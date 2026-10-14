// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <iostream>
#include <vector>
#include "common/lru_cache.h"

static void Check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
int main() {
    Common::LeastRecentlyUsedCache<int, u64> cache;
    const auto first = cache.Insert(1, 1);
    cache.Insert(2, 2);
    cache.Insert(3, 3);
    std::vector<int> seen;
    cache.ForEachItemBelow(3, [&](int value) { seen.push_back(value); return true; });
    Check(seen == std::vector<int>{1}, "bool callback stops immediately");
    seen.clear();
    cache.ForEachItemBelow(2, [&](int value) { seen.push_back(value); });
    Check(seen == std::vector<int>({1, 2}), "void callback respects the tick limit");
    seen.clear();
    cache.ForEachItemBelow(3, [&](int value) {
        seen.push_back(value);
        if (value == 1) cache.Free(first);
        return value == 2;
    });
    Check(seen == std::vector<int>({1, 2}), "freeing the current item preserves the next item and stop");
    cache.Insert(4, 4);
    seen.clear();
    cache.ForEachItemBelow(4, [&](int value) { seen.push_back(value); });
    Check(seen == std::vector<int>({2, 3, 4}), "a freed slot can be reused");
    std::cout << "LRU callback, removal and reuse checks passed\n";
}
