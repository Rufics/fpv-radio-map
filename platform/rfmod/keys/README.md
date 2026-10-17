# Development keys

These keys are **public** and exist only so the tests and local builds work out of the box.

- `dev-owner.*` stands in for the owner key when `RFMAP_OWNER_PUBKEY` is not set at build time.
  A server built that way logs a warning at start-up. Never ship such a build.
- `dev-thirdparty.*` signs the third-party test packages, which must run isolated.

For a real deployment the owner runs `rfmod keygen --out owner` on an offline machine, keeps
`owner.secret` there, and builds the server with
`-DRFMAP_OWNER_PUBKEY="$(cat owner.pub)"`.
