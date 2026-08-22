# TLS remains unsupported on Windows and Linux

The macOS portion of the original HTTPS transport gap is fixed: `core/network/tls.cb`
provides a synchronous `TlsSocket` over `Socket`, dynamically loads SecureTransport, keeps
certificate and hostname verification enabled, and has a working `https_get` smoke example.

Windows and Linux are intentionally still unsupported in this change. `TlsSocket.connect()`
returns `false` with `TLS is not implemented on this platform yet`, and the read/write surface
returns failure rather than attempting a Schannel or OpenSSL binding. The remaining work is to
add native backends for those platforms while preserving verification-on-by-default semantics.
