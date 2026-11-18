# mod_prefixlimit

Apache 2.4 module: classify clients by CIDR list (YAML), rate limit by class per URI prefix.

## Prefix list
```yaml
- prefix: 1.92.0.0/16
  class: aicrawler
```
- IPv4 and IPv6. Missing `class` = `default`.
- Subset of YAML: list of maps with `prefix`/`class`, block or flow (`- { prefix: X, class: Y }`), `#` comments, optional quotes.
- Re-read on (graceful) restart.
- Parse errors abort startup/reload (old config keeps running) and are logged with file, line, the offending text and entries read so far.
- Longest prefix wins.

## Config
```apache
LoadModule prefixlimit_module modules/mod_prefixlimit.so
PrefixListFile /etc/apache2/prefixlimit/prefixes.yaml
PrefixRateLimitStatus 429        # optional, default 429

<Location /api/>
    PrefixRateLimit aicrawler 2/s burst=4
    PrefixRateLimitRetryAfter 60
    PrefixRateLimitMessage "Too many requests, slow down."
</Location>
```
- `PrefixRateLimit <class> <n>/<s|m> [burst=N]`: token bucket, burst default = ceil(rate).
- One bucket per rule, shared by all clients of the class (not per prefix).
- Inner `<Location>` rules override outer ones for the same class.
- Over limit: 429 + `Retry-After` (seconds until a token is free).
- `PrefixRateLimitRetryAfter <seconds|auto>`: fixed `Retry-After` value instead of the computed one. Per `<Location>`, inherited. The value is only advertised, not enforced.
- `PrefixRateLimitMessage <text> [content-type]`: send this body with the 429 (default `text/plain; charset=utf-8`, e.g. `application/json`). Without it Apache's normal error page (and `ErrorDocument 429`) is used.
- On start/reload the prefix count per class is logged at `notice`:
  `LogLevel warn prefixlimit:notice`
- Env `PREFIX_CLASS` is set for matched clients (use in logs/headers).
- Uses the client IP seen by Apache: behind a proxy enable `mod_remoteip`.

## Build
```
make         # module -> .libs/mod_prefixlimit.so (apxs)
make test    # unit tests (gcc)
make itest   # local httpd test, uses /scratch/local/claude/httpd
```
Needs `apache2-dev`; `itest` also `apache2`, `curl`, `ab`.

## Debian
Native package `libapache2-mod-prefixlimit`:
```
gbp buildpackage --git-pbuilder --git-dist=bookworm
a2enmod prefixlimit
```
Edit `/etc/apache2/prefixlimit/prefixes.yaml`, then `apache2ctl graceful`.

## Limits
- Buckets reset on graceful restart.
- Only initial requests are counted.

See STATUS.md.
