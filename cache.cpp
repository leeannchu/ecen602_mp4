// cache.cpp — LRU cache of HTTP responses
#include "cache.h"
#include "http.h"
#include <cstdio>
void Cache::dump() const
{
    time_t now = time(nullptr);
    printf("[cache] %zu/%zu entries (MRU first):\n", lru_.size(), cap_);
    int i = 0;
    for (const CacheEntry &e : lru_) {
        printf("[cache]   %2d. %s (%zu bytes, %s)\n", ++i, e.url.c_str(),
               e.data.size(), is_fresh(e, now) ? "fresh" : "needs validation");
    }
}

void fill_times(CacheEntry &e, const std::string &resp, time_t now)
{
    
    std::string v; //buffer for reading header 
    // Expires
    e.expires = 0;
    if (get_header(resp, "Expires", v)) {
        e.expires = parse_http_date(v);
        if (e.expires == 0) 
            e.expires = 1;
    }
    // Last-Modified
    e.last_modified = get_header(resp, "Last-Modified", v) ? parse_http_date(v) : 0;
    // Date, if not given, use now.
    e.date = get_header(resp, "Date", v) ? parse_http_date(v) : 0;
    if (e.date == 0) e.date = now;
}

bool is_fresh(const CacheEntry &e, time_t now)
{
    //expire not given, or invalid
    if(e.expires==0||e.expires==1){
        return false; 
    }
    if (e.expires > now)
        return true;
    else
        return false;
    return false;
}

time_t ims_date(const CacheEntry &e)
{
    if(e.expires!=1&&e.expires!=0){
        return e.expires;
    }
    if (e.last_modified != 0)
        return e.last_modified;

    return e.date;
}
//look up cache entry by URL, and mark it as Most recent used
CacheEntry *Cache::lookup(const std::string &url)
{
    auto it = index_.find(url);
    if (it == index_.end())
        return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second);
    return &*it->second;
}

void Cache::insert(const std::string &url, const std::string &resp, time_t now)
{
    //If url already exists: erase the old node (and its index_ entry).
    auto f = index_.find(url);
    if (f != index_.end()) {
        lru_.erase(f->second);
        index_.erase(f);
    } else if (lru_.size() == cap_) {//else erase the oldest one
        auto &victim = lru_.back();
        printf("[cache] EVICT (LRU) %s\n", victim.url.c_str());
        index_.erase(victim.url);
        lru_.pop_back();
    }
    // Build a new cache and put it in the front
    lru_.push_front(CacheEntry{url, resp});
    fill_times(lru_.front(), resp, now);
    index_[url] = lru_.begin();
    printf("[cache] STORE %s (%zu bytes, entries %zu/%zu)\n", url.c_str(),
           resp.size(), lru_.size(), cap_);
}

void Cache::refresh(const std::string &url, const std::string &resp_304, time_t now)
{
    //if not even in the cache, skip
    CacheEntry *e = lookup(url);
    if (e == nullptr)
        return;
    // otherwise, update the cache entry with the new headers from the 304 response
    CacheEntry t;
    fill_times(t, resp_304, now);
    e->date = t.date;
    if (t.expires != 0)
        e->expires = t.expires;
    if (t.last_modified != 0)
        e->last_modified = t.last_modified;
    printf("[cache] REFRESH %s\n", url.c_str());
}
