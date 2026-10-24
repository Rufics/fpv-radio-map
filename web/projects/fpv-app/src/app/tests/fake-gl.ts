import { vi } from 'vitest';

/**
 * A WebGL2 context stand-in for jsdom, which has none: constants are numbers, the create and
 * get calls return objects, attribute locations count up, and every call is a spy.
 */
export function fakeGl(overrides: Record<string, unknown> = {}): WebGL2RenderingContext {
  let location = 0;
  const calls: Record<string, ReturnType<typeof vi.fn>> = {
    createProgram: vi.fn(() => ({})),
    createShader: vi.fn(() => ({})),
    createBuffer: vi.fn(() => ({})),
    getShaderParameter: vi.fn(() => true),
    getProgramParameter: vi.fn(() => true),
    getAttribLocation: vi.fn(() => location++),
    getUniformLocation: vi.fn(() => ({})),
    getShaderInfoLog: vi.fn(() => 'compile error'),
    getProgramInfoLog: vi.fn(() => 'link error'),
  };
  const base: Record<string, unknown> = { drawingBufferWidth: 800, drawingBufferHeight: 600, ...overrides };
  return new Proxy({} as WebGL2RenderingContext, {
    get(_, key: string) {
      if (key in base) return base[key];
      if (/^[A-Z_0-9]+$/.test(key)) return key.length;
      calls[key] ??= vi.fn();
      return calls[key];
    },
  });
}
