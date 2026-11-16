# HTTP/1.0 Caching Proxy

This project implements a simple HTTP/1.0 proxy server with an LRU cache. A
client sends an HTTP request to the proxy, and the proxy retrieves the
requested resource from the web server, returns it to the client, and stores
successful responses for later requests.

## Architecture

- **`proxy.cpp`**: Main proxy server. It uses one event loop with
  non-blocking sockets and `select()` to handle multiple client connections.
- **`client.cpp`**: Test client. It connects to the proxy, sends a URL, and
  saves the response body as `downloaded.html`.
- **`http.cpp` / `http.h`**: HTTP request, URL, header, status-code, and date
  parsing helpers.
- **`cache.cpp` / `cache.h`**: In-memory LRU cache for HTTP responses. The
  cache supports expiration and conditional requests using
  `If-Modified-Since`.

For each connection, the proxy follows this flow:

1. Read and parse the client's request.
2. Return a fresh cached response when possible.
3. Send a conditional request for a stale cache entry, or a normal request for a cache miss.
4. Relay the server response and cache HTTP 200 responses.

## Requirements

- A POSIX-compatible system such as Linux or macOS
- `g++` with C++17 support
- `make`

## Build

```bash
make
```

This builds the `proxy` and `client` executables.

## Usage

Start the proxy with a bind address and port:

```bash
./proxy <ip-to-bind> <port-to-bind>
```

Example:

```bash
./proxy 127.0.0.1 8080
```

In another terminal, run the client:

```bash
./client <proxy-address> <proxy-port> <http-url>
```

Example:

```bash
./client 127.0.0.1 8080 http://example.com/
```

The client saves the downloaded response body to `downloaded.html`.

## Errata and Limitations

- Only HTTP is supported; HTTPS and the `CONNECT` method are not implemented.
- Only `GET` requests are supported.
- The proxy currently uses IPv4 for binding and upstream connections.
- The cache has a fixed capacity of 10 entries.
- Responses without a valid `Expires` header are treated as stale and may
  require validation on every request.
- The server is designed for POSIX systems and is not directly compatible
  with native Windows sockets.

## Team Contributions

| Member | Responsibilities |
|---|---|
| **Lian Chu** | Client implementation and test report |
| **Daniel Shyu** | Proxy event loop, socket handling, request forwarding, HTTP parsing, LRU cache, conditional GET behavior, and integration |
