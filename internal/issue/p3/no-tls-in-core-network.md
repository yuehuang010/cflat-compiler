# `core/network` has no TLS, so any HTTPS data source needs an external `curl` process

Filed 2026-08-21 from an external report (a quant backtester built against v0.11.0). Feature
request, not a defect.

## The gap

`core/network` speaks plain TCP/HTTP only. Every real-world REST data source is HTTPS, so the
reporter had to shell out to the system `curl` through `process.cb` for every request - which
costs a process spawn per call, puts response parsing behind a pipe, and makes the shipped REST
examples non-runnable against any actual API.

## Fix direction

An `HttpsClient` backed by a platform TLS stack, kept behind the same interface as the existing
HTTP client so callers switch by URL scheme:

- **macOS:** Network.framework (`nw_connection` with TLS) is the current API; SecureTransport is
  deprecated but simpler. Both are reachable through the existing dlopen/objc bridge, and
  Network.framework is the better target since Security.framework's TLS surface is deprecated.
- **Windows:** Schannel, or WinHTTP for the whole client at once (WinHTTP is far less work and
  already handles proxies/redirects).
- **Linux:** OpenSSL via the C-interop header binding, which is the mechanism already used for
  other prebuilt libraries.

Scope note: certificate validation must be on by default, with no "skip verification" convenience
flag - if one is ever added it needs to be loud and per-request.

Until this lands, the REST examples should say in-file that they need an HTTPS-capable transport
and point at the `process`/`curl` workaround, so a reader does not conclude the examples are
broken.
