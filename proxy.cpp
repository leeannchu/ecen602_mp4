// proxy.cpp — HTTP/1.0 caching proxy (select loop, Conditional GET always on)
//
// Usage: ./proxy <ip to bind> <port to bind>
//
// One thread serves every connection. All sockets are non-blocking; select()
// tells us which ones are ready, and each handler does one small step and
// returns. Each client connection is a Conn that remembers where it is in
//
//   READ_REQ -> CONNECTING -> SEND_REQ -> RECV_RESP -> SEND_CLIENT -> closed
//
// (a fresh cache hit or an error jumps straight from READ_REQ to SEND_CLIENT).

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "cache.h"
#include "http.h"

#define BUFSZ 65536
#define BACKLOG 20
#define MAX_REQ 16384 // reject requests whose header is larger than this

enum class State {
    READ_REQ,    // reading the request from the client
    CONNECTING,  // non-blocking connect() to the web server in progress
    SEND_REQ,    // sending our GET to the web server
    RECV_RESP,   // reading the response from the web server (and relaying it)
    SEND_CLIENT, // server side done; flushing what's left to the client
};

struct Conn {
    int id;
    int client_fd = -1;
    int server_fd = -1;
    State state = State::READ_REQ;

    std::string req_buf; // raw request from the client
    std::string url;     // URL as sent by the client (cache key)
    Url u;               // parsed URL

    std::string server_req; // request we send to the web server
    size_t server_req_sent = 0;

    std::string resp;   // full response from the web server
    std::string out;    // bytes waiting to be written to the client
    size_t out_sent = 0;
    size_t total_to_client = 0; // for the log

    bool conditional = false; // this fetch is a Conditional GET
    bool streaming = false;   // relay server bytes to client as they arrive
    bool dead = false;        // marked for removal at the end of the loop
};

static std::vector<Conn *> conns;
static Cache cache(10);
static int next_id = 1;

static bool would_block(int err)
{
    return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
}
static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl");
        exit(1);
    }
}

static int make_listener(const char *ip, const char *port)
{
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int rv = getaddrinfo(ip, port, &hints, &res);
    if (rv != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        exit(1);
    }
    int fd = -1;
    for (p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        if (bind(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        fprintf(stderr, "proxy: failed to bind %s:%s\n", ip, port);
        exit(1);
    }
    if (listen(fd, BACKLOG) < 0) {
        perror("listen");
        exit(1);
    }
    set_nonblocking(fd);
    printf("[proxy] listening on %s:%s\n", ip, port);
    return fd;
}

static void accept_client(int lfd)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    int fd = accept(lfd, (struct sockaddr *)&ss, &len);
    if (fd < 0) {
        if (!would_block(errno)) perror("accept");
        return;
    }
    set_nonblocking(fd);
    Conn *c = new Conn;
    c->id = next_id++;
    c->client_fd = fd;
    conns.push_back(c);

    char ip[INET6_ADDRSTRLEN] = "?";
    if (ss.ss_family == AF_INET)
        inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, ip, sizeof ip);
    printf("[conn %d] new client from %s (fd %d)\n", c->id, ip, fd);
}

static void close_server(Conn *c)
{
    if (c->server_fd >= 0) {
        close(c->server_fd);
        c->server_fd = -1;
    }
}

static void close_conn(Conn *c)
{
    close_server(c);
    if (c->client_fd >= 0) {
        close(c->client_fd);
        c->client_fd = -1;
    }
    c->dead = true;
    printf("[conn %d] closed\n", c->id);
}

// Queue data for the client.
static void queue_out(Conn *c, const std::string &data)
{
    c->out.append(data);
}

// Reply with an error page and finish.
static void send_error(Conn *c, int code, const char *reason)
{
    printf("[conn %d] error %d %s\n", c->id, code, reason);
    close_server(c);
    std::string body = "<html><body><h1>" + std::to_string(code) + " " + reason +
                       "</h1></body></html>\n";
    std::string r = "HTTP/1.0 " + std::to_string(code) + " " + reason + "\r\n" +
                    "Content-Type: text/html\r\n" +
                    "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    c->out.clear();
    c->out_sent = 0;
    queue_out(c, r);
    c->state = State::SEND_CLIENT;
}
static void start_fetch(Conn *c, const CacheEntry *stale);
static void finish_fetch(Conn *c);

// Tell select() what this Conn is waiting for, based on its state.
// The client goes into wfds during RECV_RESP only when there is something
// to send; a socket is almost always writable, so adding it with nothing
// to send would make select() return immediately forever (busy loop).
static void add_fds(Conn *c, fd_set &rfds, fd_set &wfds, int &maxfd)
{
    auto R = [&](int fd) { FD_SET(fd, &rfds); maxfd = std::max(maxfd, fd); };
    auto W = [&](int fd) { FD_SET(fd, &wfds); maxfd = std::max(maxfd, fd); };

    switch (c->state) {
    case State::READ_REQ:
        R(c->client_fd); // wait for the client's request
        break;
    case State::CONNECTING: // a non-blocking connect() finishes -> socket becomes writable
    case State::SEND_REQ:   // we can send our request to the server
        W(c->server_fd);
        break;
    case State::RECV_RESP:
        R(c->server_fd);                 // more data from the server
        if (c->out_sent < c->out.size()) // something waiting for the client
            W(c->client_fd);
        break;
    case State::SEND_CLIENT:
        W(c->client_fd); // flush the rest
        break;
    }
}

//The whole request header has arrived: decide what to do.
//     fresh cache hit  -> answer from the cache, no server contact
//     stale cache hit  -> Conditional GET (If-Modified-Since)
//     miss             -> normal GET
static void handle_request(Conn *c)
{
    std::string method;
    if (!parse_request_line(c->req_buf, method, c->url)) {
        send_error(c, 400, "Bad Request");
        return;
    }
    if (method != "GET") {
        send_error(c, 501, "Not Implemented");
        return;
    }
    if (!parse_url(c->url, c->u)) {
        send_error(c, 400, "Bad Request");
        return;
    }
    printf("[conn %d] request: GET %s\n", c->id, c->url.c_str());

    time_t now = time(nullptr);
    CacheEntry *e = cache.lookup(c->url); // also marks it most-recently-used
    if (e && is_fresh(*e, now)) {
        printf("[cache] HIT (fresh, Expires in %lds) %s\n", (long)(e->expires - now),
               c->url.c_str());
        queue_out(c, e->data); // copy: the entry may be evicted while we send
        c->state = State::SEND_CLIENT;
    } else if (e) {
        printf("[cache] STALE (%s) -> Conditional GET %s\n",
               e->expires ? "Expires passed" : "no Expires", c->url.c_str());
        start_fetch(c, e);
    } else {
        printf("[cache] MISS %s\n", c->url.c_str());
        start_fetch(c, nullptr);
    }
}

//Client readable while in READ_REQ: accumulate the request until we
//     have the full header ("\r\n\r\n"). TCP may deliver it in pieces.
static void on_client_readable(Conn *c)
{
    char buf[BUFSZ];
    ssize_t n = recv(c->client_fd, buf, sizeof buf, 0);
    if (n < 0 && would_block(errno)) return;
    if (n <= 0) { // 0 = client closed before sending a full request
        if (n < 0) printf("[conn %d] recv(client): %s\n", c->id, strerror(errno));
        close_conn(c);
        return;
    }
    c->req_buf.append(buf, n);

    if (header_end(c->req_buf) != std::string::npos)
        handle_request(c);
    else if (c->req_buf.size() > MAX_REQ)
        send_error(c, 400, "Bad Request");
}

// Resolve the host, start a non-blocking connect, and build the
// request we will send. If `stale` is given, this is a Conditional GET.
static void start_fetch(Conn *c, const CacheEntry *stale)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int rv = getaddrinfo(c->u.host.c_str(), c->u.port.c_str(), &hints, &res);
    if (rv != 0) {
        printf("[conn %d] getaddrinfo(%s): %s\n", c->id, c->u.host.c_str(), gai_strerror(rv));
        send_error(c, 502, "Bad Gateway");
        return;
    }

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, ip, sizeof ip);

    // non-blocking connect
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        perror("socket");
        freeaddrinfo(res);
        send_error(c, 502, "Bad Gateway");
        return;
    }
    set_nonblocking(fd);
    c->server_fd = fd;
    printf("[conn %d] connecting to %s:%s (%s)\n", c->id, c->u.host.c_str(),
           c->u.port.c_str(), ip);

    int r = connect(fd, res->ai_addr, res->ai_addrlen);
    int err = errno;
    freeaddrinfo(res);
    if (r == 0) {
        c->state = State::SEND_REQ; // connected immediately (can happen on localhost)
    } else if (err == EINPROGRESS) {
        c->state = State::CONNECTING; // finish later; select tells us when writable
    } else {
        printf("[conn %d] connect: %s\n", c->id, strerror(err));
        send_error(c, 502, "Bad Gateway");
        return;
    }

    // --- request to the web server: only the path, not the full URL ---
    c->server_req = "GET " + c->u.path + " HTTP/1.0\r\n" +
                    "Host: " + c->u.host + "\r\n" +
                    "Connection: close\r\n";
    c->server_req_sent = 0;
    c->conditional = (stale != nullptr);
    if (c->conditional) {
        std::string d = format_http_date(ims_date(*stale));
        c->server_req += "If-Modified-Since: " + d + "\r\n";
        printf("[conn %d] If-Modified-Since: %s\n", c->id, d.c_str());
    }
    c->server_req += "\r\n";

    // For a normal GET we relay bytes as they arrive. For a Conditional GET we
    // must see the status line first: a 304 must NOT be relayed to the client.
    c->streaming = !c->conditional;
}

// Server writable: first confirm the connect succeeded, then send our
//     request (possibly over several calls if send() only takes part of it).
static void on_server_writable(Conn *c)
{
    if (c->state == State::CONNECTING) {
        int err = 0;
        socklen_t len = sizeof err;
        if (getsockopt(c->server_fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) err = errno;
        if (err != 0) {
            printf("[conn %d] connect failed: %s\n", c->id, strerror(err));
            send_error(c, 502, "Bad Gateway");
            return;
        }
        printf("[conn %d] connected to web server\n", c->id);
        c->state = State::SEND_REQ;
    }

    while (c->server_req_sent < c->server_req.size()) {
        ssize_t n = send(c->server_fd, c->server_req.data() + c->server_req_sent,
                         c->server_req.size() - c->server_req_sent, 0);
        if (n < 0) {
            if (would_block(errno)) return; // send buffer full; try next round
            printf("[conn %d] send(server): %s\n", c->id, strerror(errno));
            send_error(c, 502, "Bad Gateway");
            return;
        }
        c->server_req_sent += n;
    }
    printf("[conn %d] request sent to web server\n", c->id);
    c->state = State::RECV_RESP;
}

//Server readable: collect the response. EOF means it is complete
static void on_server_readable(Conn *c)
{
    char buf[BUFSZ];
    ssize_t n = recv(c->server_fd, buf, sizeof buf, 0);
    if (n < 0) {
        if (would_block(errno)) return;
        printf("[conn %d] recv(server): %s\n", c->id, strerror(errno));
        if (!c->streaming) send_error(c, 502, "Bad Gateway"); // client got nothing yet
        else close_conn(c);                                   // half-sent; just cut it
        return;
    }
    if (n == 0) {
        finish_fetch(c);
        return;
    }

    c->resp.append(buf, n); // keep everything for the cache
    if (c->streaming) {
        queue_out(c, std::string(buf, n)); // and relay it right away
    } else if (c->conditional && header_end(c->resp) != std::string::npos) {
        // Conditional GET: the status line has arrived, now we can decide.
        int status = parse_status_code(c->resp);
        if (status != 304) {
            printf("[conn %d] status %d -> document changed, relaying new copy\n", c->id,
                   status);
            c->streaming = true;
            queue_out(c, c->resp); // everything received so far
        }
        // 304: keep buffering (it's just a header); handled at EOF
    }
}

//The web server closed the connection: the response is complete.
static void finish_fetch(Conn *c)
{
    close_server(c);
    int status = parse_status_code(c->resp);
    printf("[conn %d] web server closed: %zu bytes, status %d\n", c->id, c->resp.size(),
           status);
    time_t now = time(nullptr);

    if (c->conditional && status == 304) {
        // Not modified: the cached copy is still good.
        cache.refresh(c->url, c->resp, now);
        CacheEntry *e = cache.lookup(c->url);
        if (e) {
            printf("[conn %d] 304 Not Modified -> serving cached copy\n", c->id);
            queue_out(c, e->data);
        } else {
            // The entry was evicted while we were waiting. A 304 alone means
            // nothing to the client, so fetch the whole document again.
            printf("[conn %d] cached copy was evicted meanwhile, refetching\n", c->id);
            c->resp.clear();
            start_fetch(c, nullptr);
            return;
        }
    } else {
        if (!c->streaming) queue_out(c, c->resp); // anything we held back
        if (status == 200) {
            cache.insert(c->url, c->resp, now);
            cache.dump();
        } else {
            printf("[cache] NOT CACHED (status %d) %s\n", status, c->url.c_str());
        }
    }
    c->state = State::SEND_CLIENT;
}

//Client writable: send as much of `out` as the socket accepts.
//     When everything is sent and the server side is finished, we are done.
static void on_client_writable(Conn *c)
{
    while (c->out_sent < c->out.size()) {
        ssize_t n = send(c->client_fd, c->out.data() + c->out_sent,
                         c->out.size() - c->out_sent, 0);
        if (n < 0) {
            if (would_block(errno)) return; // client is slow; wait for select
            printf("[conn %d] send(client): %s\n", c->id, strerror(errno));
            close_conn(c); // e.g. EPIPE: the client went away
            return;
        }
        c->out_sent += n;
        c->total_to_client += n;
    }
    c->out.clear(); // all sent: reset the buffer so it doesn't keep growing
    c->out_sent = 0;

    if (c->state == State::SEND_CLIENT) {
        printf("[conn %d] done, sent %zu bytes to client\n", c->id, c->total_to_client);
        close_conn(c);
    }
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "usage: proxy <ip to bind> <port to bind>\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);            // a closed client must not kill the proxy
    setvbuf(stdout, nullptr, _IOLBF, 0); // line-buffered logs even when redirected

    int lfd = make_listener(argv[1], argv[2]);

    for (;;) {
        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_SET(lfd, &rfds);
        int maxfd = lfd;
        for (Conn *c : conns) add_fds(c, rfds, wfds, maxfd);

        if (select(maxfd + 1, &rfds, &wfds, nullptr, nullptr) < 0) {
            if (errno == EINTR) continue;
            perror("select");
            return 1;
        }

        if (FD_ISSET(lfd, &rfds)) accept_client(lfd);

        // Handlers may create new server sockets whose fd number was just
        // freed this round, so they must tolerate a spurious EAGAIN.
        for (size_t i = 0; i < conns.size(); i++) {
            Conn *c = conns[i];
            if (!c->dead && c->state == State::READ_REQ && FD_ISSET(c->client_fd, &rfds))
                on_client_readable(c);
            if (!c->dead && c->server_fd >= 0 && FD_ISSET(c->server_fd, &wfds) &&
                (c->state == State::CONNECTING || c->state == State::SEND_REQ))
                on_server_writable(c);
            if (!c->dead && c->server_fd >= 0 && FD_ISSET(c->server_fd, &rfds) &&
                c->state == State::RECV_RESP)
                on_server_readable(c);
            if (!c->dead && c->client_fd >= 0 && FD_ISSET(c->client_fd, &wfds))
                on_client_writable(c);
        }

        // Remove finished connections.
        auto mid = std::stable_partition(conns.begin(), conns.end(),
                                         [](Conn *c) { return !c->dead; });
        for (auto it = mid; it != conns.end(); ++it) delete *it;
        conns.erase(mid, conns.end());
    }
}
