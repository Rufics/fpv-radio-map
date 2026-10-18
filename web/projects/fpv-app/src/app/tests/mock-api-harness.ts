import { provideHttpClient, withInterceptors } from '@angular/common/http';
import { TestBed } from '@angular/core/testing';
import { vi } from 'vitest';
import { mockApiInterceptor } from '../mock/mock-api.interceptor';

/** The app's HTTP stack answered by the dev mock backend, with its latency on fake timers. */
export class MockApiHarness {
  static setUp(): void {
    vi.useFakeTimers();
    TestBed.configureTestingModule({ providers: [provideHttpClient(withInterceptors([mockApiInterceptor]))] });
  }

  static tearDown(): void {
    vi.useRealTimers();
  }

  /** Runs simulated time forward until `p` settles (at most `limitMs`), and returns its outcome. */
  static async settle<T>(p: Promise<T>, limitMs = 10_000): Promise<T> {
    let done = false;
    const watched = p.finally(() => {
      done = true;
    });
    watched.catch(() => undefined);
    for (let t = 0; !done && t < limitMs; t += 50) await vi.advanceTimersByTimeAsync(50);
    return watched;
  }

  /** Moves simulated time forward by `ms`. */
  static async wait(ms: number): Promise<void> {
    await vi.advanceTimersByTimeAsync(ms);
  }
}
