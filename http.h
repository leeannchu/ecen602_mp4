// http.h — HTTP/1.0 parsing helpers (URL, request line, headers, dates)
#pragma once
#include <ctime>
#include <string>

struct Url {
    std::string host;
    std::string port; // "80" if not given
    std::string path; // always starts with '/'
};

// Returns the index just past "\r\n\r\n", or std::string::npos if the header block is not complete yet.
size_t header_end(const std::string &buf);

// time_t (UTC)
std::string format_http_date(time_t t);

// "http://host[:port][/path]" or "host[:port][/path]" -> Url
// Returns false if the host is empty or the scheme is not http.
bool parse_url(const std::string &url, Url &out);

// "GET http://a/b HTTP/1.0\r\n..." -> method = "GET", url = "http://a/b"
bool parse_request_line(const std::string &req, std::string &method, std::string &url);

// Look up a header in the header block (everything before "\r\n\r\n").
// Name match is case-insensitive; leading/trailing spaces of the value are
// trimmed. Returns false if the header is absent.
bool get_header(const std::string &msg, const std::string &name, std::string &value);

// "HTTP/1.0 200 OK\r\n..." -> 200. Returns -1 if malformed.
int parse_status_code(const std::string &resp);

// Accepts the three formats in RFC 1945 §3.3 and returns time_t in UTC.
// Returns 0 if the string cannot be parsed.
time_t parse_http_date(const std::string &s);
