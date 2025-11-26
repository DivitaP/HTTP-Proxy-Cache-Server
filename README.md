# HTTP Proxy Cache Server

## Description
This project is an HTTP proxy server in C that forwards GET requests, caches responses for faster access, prefetches links from HTML pages in background threads, and blocks specified sites using regex patterns. It supports keep-alive connections and handles errors like bad requests or forbidden sites.

## Features
- **Proxy Forwarding**: Parses and forwards GET requests to origin servers.
- **Caching**: Stores responses in `./cache` with timeout; checks validity before serving.
- **Prefetching**: Background threads prefetch href links from HTML for proactive caching.
- **Blocklist**: Regex-based blocking of sites (hardcoded, e.g., google.com, facebook.com).
- **Error Handling**: Sends HTTP errors (400, 403, 404, etc.); logs to console.
- **Graceful Shutdown**: Ctrl+C clears cache directory.

## Assumptions
- Only prefetches `href` links; ignores other tags like `src`.
- Blocklist is hardcoded in code; not loaded from file.
- Cache dir `./cache`; max 1000 entries, filenames based on host/path.
- Supports HTTP/1.1; assumes GET only.
- Timeout specified at startup for cache expiration.

## Compilation
```bash
gcc proxy.c -o proxy_server -lpthread
```

## Usage
Run with port and cache timeout (seconds):
```bash
./proxy_server <port> <timeout>
```
Example:
```bash
./proxy_server 8080 60
```
Configure browser/proxy client to use localhost:8080.

## Example
- Request `http://example.com` via proxy: Cached on first access, served from cache subsequently.
- Prefetch: HTML links are fetched in background.
- Blocked site: Access to google.com returns 403 Forbidden.

## Dependencies
- POSIX threads (`-lpthread`).

## Notes
- Cache files are removed on shutdown.
- For debugging, check console for cache hits/misses and prefetch logs.
