# Security Policy

## Supported versions

Only the latest release receives fixes.

## Reporting a vulnerability

Please report security issues privately through
[GitHub Security Advisories](https://github.com/iAlturki/ytr-music/security/advisories/new)
rather than a public issue. Include the version, steps to reproduce and the impact.
You will get a reply within a few days.

## Notes

- Release builds never open a DevTools/remote-debugging port. It is only enabled when
  the app is started with `--debug`, and then only on `127.0.0.1`.
- Browsing data (cookies, cache) is stored per user in `%APPDATA%\ytr-music-native`.
