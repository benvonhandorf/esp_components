# http_server

One HTTP server, routes registered as data, optional Basic authentication per route.

```c
static const http_route_t routes[] = {
    {"/api/status", HTTP_GET,  handle_status, &ctx, false},  /* open */
    {"/api/config", HTTP_POST, handle_config, NULL,  true},  /* authenticated */
};
http_server_add_routes(routes, 2);
http_server_start(&cfg);      /* listens once the network is up */
```

Routes may be registered before or after `start()`, and by components as well as by the
application. This replaces a weak `http_server_register_services()` the application
overrode with a strong symbol — link-order dependent, and no use to a component wanting to
serve a route of its own.

A handler gets its context with `http_server_route_ctx(req)`.

## Share the server

```c
cli_web_config_t web = { .server = http_server_handle(), .page_uri = "/console" };
cli_web_start(&web);
```

Two listeners on one device is a bug, not a feature.

## Authentication

`require_auth` is **per route**: a status endpoint a dashboard scrapes and an endpoint
that reflashes the device do not warrant the same treatment. Every route goes through one
trampoline, so the decision is made in one place rather than depending on each handler
remembering to ask.

**Starting with `require_auth` and no password is refused** (`HTTP_SERVER_ERR_NO_PASSWORD`).
An interface that can reflash a device must not be reachable with credentials that ship in
the source, and quietly disabling authentication instead would be worse than failing to
start. The schema therefore gives `password` no default, and `require_auth` defaults to
**true** — the predecessor defaulted it to false with `admin`/`admin`.

### The credential check never parses what the client sent

The expected `Authorization` value is built **once at start** by Base64-encoding our own
credentials, and the header is compared against it. Nothing attacker-controlled is
decoded, so a Base64 decoder — and its handling of invalid characters, bad padding and
short groups — is not in the attack surface at all. Base64 has one canonical encoding per
input and RFC 7617 requires clients to use it, so comparing encodings is equivalent to
comparing credentials.

The comparison examines every byte regardless of where the first difference falls, so the
number of correct leading characters cannot be recovered by timing repeated requests. The
predecessor hand-rolled a Base64 decoder over the header and finished with `strcmp`.

## Tests

```sh
make -C test
```

Encoding and comparison are pure logic and are tested on the host, including the RFC 4648
vectors and the prefix case a short-circuiting comparison gets wrong.
