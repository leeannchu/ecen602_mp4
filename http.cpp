// http.cpp — HTTP/1.0 parsing helpers
#include "http.h"

#include <cctype>
#include <cstring>
#include <strings.h> 
#include <time.h>    
#include <sstream>
#include <cstdio>
size_t header_end(const std::string &buf)
{
    size_t p = buf.find("\r\n\r\n");
    return p == std::string::npos ? std::string::npos : p + 4;
}

std::string format_http_date(time_t t)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return buf;
}


bool parse_url(const std::string &url, Url &out)
{
    std::string s = url; 
    // If url starts with "http://", skip it. 
    //If it contains "://" with a  different scheme (e.g. https://), return false.
    if (s.find("://") != std::string::npos) {
        if (strncasecmp(s.c_str(), "http://", 7) != 0) 
            return false; //unvalid url
        s = s.substr(7);        
    }
    //  2. host[:port] ends at the first of "/?#" (or end of string).
    size_t e = s.find_first_of("/?#");
    std::string hostport = s.substr(0, e); // e 是 npos 時會取到結尾，剛好
    std::string path = (e == std::string::npos) ? "" : s.substr(e);
    //  3. Split host and port on ':'. No port -> "80".
    size_t colon = hostport.find(':');
    out.host = hostport.substr(0, colon);
    out.port = (colon == std::string::npos) ? "80" : hostport.substr(colon + 1);
    if (out.port.empty()) out.port = "80";
    //  4. path = the rest; if empty -> "/"; if it starts with '?' -> prepend '/'.
    if (path.empty()) path = "/";
    else if (path[0] != '/') path = "/" + path;   // "?x=1" -> "/?x=1"
    out.path = path;
    //  5. Empty host -> return false.
    return !out.host.empty();

}

bool parse_request_line(const std::string &req, std::string &method, std::string &url)
{
    std::string line = req.substr(0, req.find("\r\n"));
    std::istringstream iss(line);
    std::string version;
    if (!(iss >> method >> url >> version)) 
        return false;   
    return version.compare(0, 5, "HTTP/") == 0;

}

bool get_header(const std::string &msg, const std::string &name, std::string &value)
{
    size_t end = msg.find("\r\n\r\n");
    if (end == std::string::npos) 
        end = msg.size();  
    size_t pos = msg.find("\r\n");                  
    if (pos == std::string::npos) 
        return false;

    while (true) {
        size_t ls = pos + 2;                          
        if (ls > end) break;                          
        size_t le = msg.find("\r\n", ls);             
        if (le == std::string::npos || le > end) le = end;
        std::string line = msg.substr(ls, le - ls);

        if (line.size() > name.size() &&
            line[name.size()] == ':' &&
            strncasecmp(line.c_str(), name.c_str(), name.size()) == 0) {

            std::string v = line.substr(name.size() + 1);
            size_t f = v.find_first_not_of(" \t");
            size_t l = v.find_last_not_of(" \t");
            value = (f == std::string::npos) ? "" : v.substr(f, l - f + 1);  
            return true;
        }
        if (le == end) break;
        pos = le;
    }
    return false;
}

int parse_status_code(const std::string &resp)
{
    int code;
    if (sscanf(resp.c_str(), "HTTP/%*d.%*d %d", &code) != 1) 
        return -1;
    return (code >= 100 && code <= 999) ? code : -1;
}

time_t parse_http_date(const std::string &s)
{
    const char *fmts[] = {
        "%a, %d %b %Y %H:%M:%S GMT",   // RFC 1123
        "%A, %d-%b-%y %H:%M:%S GMT",   // RFC 850
        "%a %b %d %H:%M:%S %Y",        // asctime
    };
    for (const char *f : fmts) {
        struct tm tm;
        memset(&tm, 0, sizeof tm);          // 一定要清零，strptime 不會填沒出現的欄位
        if (strptime(s.c_str(), f, &tm) != NULL)
            return timegm(&tm);
    }
    return 0;
}
