# FPV Radio Map documentation

FPV Radio Map answers one question before you fly: will the control and video link reach the target, and if not, why?

This folder is the project's documentation. It is reviewed in pull requests like code, and every change on `main` is published to the [wiki](https://github.com/Rufics/fpv-radio-map/wiki) automatically. Edit the files here, not the wiki pages.

## Using it

- [Glossary](guide/glossary.md): the radio terms the app uses, in plain words.
- [FAQ](guide/faq.md): questions that come up again and again.
- [Field reports](community/field-reports.md): flights that were compared with a prediction.

## Running it

- [Quick start](../README.md#quick-start)
- [Production server, step by step](admin/production-setup.md)
- [Deployment](admin/deploy.md)
- [Building map data](admin/tiles-builder.md)
- [How map layers reach the browser](admin/map-layers.md)
- [How the tiles builder generates map data](admin/map-generation.md)
- [How the weather forecast reaches the pilot](admin/weather-data.md)

## Extending it

- [Writing a module](modules/developer-guide.md)
- [HTTP API (OpenAPI)](api/openapi.yaml)
- [Database migrations](dev/migrations.md): how the schema and the data move to a new version, and how
  to write a migration.
- [Fuzz testing](dev/fuzzing.md): how the C++ input parsers are fuzzed.
- [SonarQube Cloud](dev/sonarcloud.md): what the static analysis covers and how it is set up.
- [Contributing](../CONTRIBUTING.md)
- [Repository settings](maintainers/repository-settings.md): the GitHub settings the CI and the review rules rely on.

## Where to ask

- Questions and ideas: [Discussions](https://github.com/Rufics/fpv-radio-map/discussions).
- Bugs and wrong equipment data: [Issues](https://github.com/Rufics/fpv-radio-map/issues/new/choose).
- Security problems: [report privately](https://github.com/Rufics/fpv-radio-map/security/advisories/new).
