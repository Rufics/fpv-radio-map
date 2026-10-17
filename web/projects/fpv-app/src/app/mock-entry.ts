import type * as MockSetup from './mock/mock-setup';

/**
 * Production: no dev mock. The `development` and `mock` build configurations replace this file
 * with mock-entry.mock.ts (see angular.json), so production bundles contain no mock code.
 */
export const loadMock: (() => Promise<typeof MockSetup>) | null = null;
