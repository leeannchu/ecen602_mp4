// cache.h — LRU cache of HTTP responses (Bonus / Conditional GET mode)
#pragma once
#include <ctime>
#include <list>
#include <string>
#include <unordered_map>

struct CacheEntry {
    std::string url;
    std::string data;        // full response: status line + headers + body
    time_t expires = 0;      // 0 = no Expires header
    time_t last_modified = 0;// 0 = no Last-Modified header
    time_t date = 0;         // last time we got this doc from the server (Date header, or local time)
};

class Cache {
public:
    explicit Cache(size_t capacity = 10) : cap_(capacity) {}

    // Returns the entry and marks it most-recently-used, or nullptr.
    // The pointer is valid until the next insert().
    CacheEntry *lookup(const std::string &url);

    // Store a 200 response. Replaces an existing entry for the same URL;
    // otherwise evicts the LRU entry when full.
    void insert(const std::string &url, const std::string &resp, time_t now);

    // After a 304 Not Modified: keep the cached data, update the entry's
    // date (and expires / last_modified if the 304 carries new values).
    void refresh(const std::string &url, const std::string &resp_304, time_t now);

    size_t size() const { return lru_.size(); }
    void dump() const; // provided: print all entries, MRU first

private:
    size_t cap_;
    std::list<CacheEntry> lru_; // front = most recently used
    std::unordered_map<std::string, std::list<CacheEntry>::iterator> index_;
};

// Fill expires / last_modified / date from the response headers.
void fill_times(CacheEntry &e, const std::string &resp, time_t now);

// Bonus : chech freshness
bool is_fresh(const CacheEntry &e, time_t now);

// Date for If-Modified-Since: Expires, else Last-Modified, else Date.
time_t ims_date(const CacheEntry &e);
