// Conventional Commits, see CONTRIBUTING.md. The version bump of a release comes from these types.
export default {
  extends: ['@commitlint/config-conventional'],
  rules: {
    'scope-enum': [
      2,
      'always',
      ['rf-core', 'rf-ext', 'rf-flightlog', 'rfmod', 'fpv', 'server', 'web', 'tiles', 'deploy', 'data', 'schemas', 'docs', 'ci', 'perf', 'deps'],
    ],
    'subject-case': [0],
    'body-max-line-length': [0],
    'footer-max-line-length': [0],
  },
};
