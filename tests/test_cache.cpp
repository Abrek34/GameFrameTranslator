#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>

#include "cache/sqlite_cache.hpp"

using namespace gft;

int main() {
    const std::string path = "/tmp/gft_test_cache.sqlite3";
    std::filesystem::remove(path);
    {
        TranslationCache c(path, 8);
        if (c.get("en", "tr", "Continue").has_value()) { std::cerr << "unexpected hit\n"; return 1; }
        c.put("en", "tr", "Continue", "Devam", "dictionary");
        auto h = c.get("en", "tr", "Continue");
        if (!h || h->first != "Devam") { std::cerr << "memory get failed\n"; return 1; }
        auto h2 = c.get("en", "tr", "  Continue  ");
        if (!h2 || h2->first != "Devam") { std::cerr << "normalize get failed\n"; return 1; }
        c.close();
    }
    {
        TranslationCache c(path, 8);
        auto h = c.get("en", "tr", "Continue");
        if (!h || h->first != "Devam") { std::cerr << "disk get failed\n"; return 1; }
    }
    std::filesystem::remove(path);
    std::cout << "test_cache OK\n";
    return 0;
}
