claude --dangerously-skip-permissions

For REST API support in CFlat, here's the natural layering from bottom to top:

  Layer 1 — Transport (Foundation)                                                                                                                            
  ┌───────────┬────────────────────────────────────────────────────────────────────────────────────────────────────────────┐                                    │  Library  │                                                Description                                                 │
  ├───────────┼────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
  │ socket.cb │ Winsock2 bindings: connect, bind, listen, accept, send, recv, closesocket; WSAStartup/WSACleanup lifecycle │
  └───────────┴────────────────────────────────────────────────────────────────────────────────────────────────────────────┘

  Layer 2 — HTTP Protocol

  ┌──────────────────┬──────────────────────────────────────────────────────────────────────────────────────────────────┐
  │     Library      │                                           Description                                            │
  ├──────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────┤
  │ http_parser.cb   │ Parse raw bytes into request line, headers map, body; handle Content-Length and chunked transfer │
  ├──────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────┤
  │ http_response.cb │ Build response: status line, headers, body; helpers for common codes (200, 404, 500)             │
  └──────────────────┴──────────────────────────────────────────────────────────────────────────────────────────────────┘

  Layer 3 — Server / Client

  ┌────────────────┬────────────────────────────────────────────────────────────────────────────┐
  │    Library     │                                Description                                 │
  ├────────────────┼────────────────────────────────────────────────────────────────────────────┤
  │ http_server.cb │ Accept loop (uses thread.cb), dispatch connections to handlers, keep-alive │
  ├────────────────┼────────────────────────────────────────────────────────────────────────────┤
  │ http_client.cb │ Outbound GET/POST/PUT/DELETE; follows redirects; returns response struct   │
  └────────────────┴────────────────────────────────────────────────────────────────────────────┘

  Layer 4 — REST Convenience

  ┌────────────────┬────────────────────────────────────────────────────────────────────────────────────────┐
  │    Library     │                                      Description                                       │
  ├────────────────┼────────────────────────────────────────────────────────────────────────────────────────┤
  │ router.cb      │ Route table: GET /users/:id style matching, path param extraction                      │
  ├────────────────┼────────────────────────────────────────────────────────────────────────────────────────┤
  │ rest_server.cb │ Thin wrapper: register handlers by method+path, auto-serialize/deserialize via json.cb │
  └────────────────┴────────────────────────────────────────────────────────────────────────────────────────┘

  Already done

  - json.cb — serialization/deserialization ✓
  - string.cb — string manipulation ✓
  - thread.cb — per-connection threads ✓
  - channel.cb — work queue for connection dispatch ✓

  Suggested build order

  1. socket.cb — everything else depends on it
  2. http_parser.cb + http_response.cb
  3. http_client.cb (simpler, good test bed for the parser)
  4. http_server.cb
  5. router.cb + rest_server.cb

  HTTPS (TLS via Schannel or a bundled mbedTLS binding) would sit between layers 1 and 2, but is optional for an initial implementation.