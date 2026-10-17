import type * as MockSetup from './mock/mock-setup';

/** Dev mock enabled: loaded lazily by main.ts. */
export const loadMock: (() => Promise<typeof MockSetup>) | null = () => import('./mock/mock-setup');
