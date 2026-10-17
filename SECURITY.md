# Security policy

## Supported versions

Until 1.0, only the latest release receives security fixes.

## Reporting a vulnerability

Please **do not open a public issue**. Report it privately through
[GitHub private vulnerability reporting](https://github.com/Rufics/fpv-radio-map/security/advisories/new),
with the affected version or commit, the steps to reproduce, and the impact you expect.

We aim to acknowledge a report within 7 days, and we'll tell you how we plan to fix it and when we will publish an
advisory. Credit is given in the advisory unless you prefer otherwise.

## Especially in scope

- **Module isolation:** a third-party `.rfmod` that escapes its worker process, reads files, uses the network, or
  affects other modules or the server.
- **Package signing:** a package that passes signature or integrity checks without a valid key, or a way to make
  the server trust a key the admin did not add.
- **Share links:** bypassing the PIN or its rate limit, reading a mission without a valid link, or revoking a link
  without the owner key.
- **Admin API:** any admin action without the admin token, including library and module changes.

## Keys

The keys in `platform/rfmod/keys/` are public **development** keys and protect nothing. Real deployments are built
with their own owner key (`RFMAP_OWNER_PUBKEY`); release modules are signed in CI with a key kept as a repository
secret. A report that relies only on the development keys is not a vulnerability.
