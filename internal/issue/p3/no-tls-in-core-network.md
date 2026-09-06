# TLS remains unsupported on Linux

The macOS portion of the original HTTPS transport gap is fixed: `core/network/tls.cb`
provides a synchronous `TlsSocket` over `Socket`, dynamically loads SecureTransport, keeps
certificate and hostname verification enabled, and has a working `https_get` smoke example.

Windows landed in this change via the runtime-loaded Schannel backend in
`core/network/tls.cb` and `core/network/tls.windows.cb`, with certificate and hostname
verification enabled by default. Linux remains unsupported: `TlsSocket.connect()` returns
`false` with `TLS is not implemented on this platform yet`, and the read/write surface returns
failure rather than attempting an OpenSSL binding. The remaining work is the Linux backend.

Accepted limitation on Windows: `close()` does not send a TLS `close_notify` alert (no
`ApplyControlToken` / `SCHANNEL_SHUTDOWN` round). The connection is dropped with a TCP FIN.
This is safe for a synchronous request/response client, which reads until the peer's own close
and so cannot be fooled by a truncated response; the cost is that a peer may log an unclean
shutdown and may decline to cache the session for resumption. Revisit if `TlsSocket` grows
session reuse or long-lived bidirectional streams.
