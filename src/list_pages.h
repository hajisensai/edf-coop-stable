#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dn {
struct ListPages {
    uint32_t version = 0;
    uint16_t total = 0;
    uint32_t applied = 0;
    std::map<uint16_t, std::vector<std::string>> pages;
};
inline bool currentSinglePage(const ListPages& pages, uint32_t version) {
    return version >= pages.applied && version >= pages.version;
}
inline bool applyListPage(ListPages& pages, uint32_t version, uint16_t total, uint16_t offset,
                          const std::vector<std::string>& entries, std::vector<std::string>& out) {
    // 1024 slots + 1024 frozen participants + removed IDs and metadata. Never allocate from an unbounded total.
    if (!total || total > 4096 || entries.empty() || static_cast<size_t>(offset) + entries.size() > total ||
        version <= pages.applied || version < pages.version) return false;
    if (version != pages.version) {
        pages.version = version;
        pages.total = total;
        pages.pages.clear();
    }
    if (total != pages.total) return false;
    pages.pages[offset] = entries;
    size_t next = 0;
    for (const auto& [at, page] : pages.pages) {
        if (at != next) return false; // missing or overlapping offsets cannot produce a valid admission manifest
        next += page.size();
    }
    if (next != total) return false;
    out.clear();
    for (const auto& [at, page] : pages.pages) {
        (void)at;
        out.insert(out.end(), page.begin(), page.end());
    }
    pages.applied = version;
    pages.pages.clear();
    return true;
}
}
