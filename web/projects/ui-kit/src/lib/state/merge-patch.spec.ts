import { applyMergePatch } from './merge-patch';

describe('applyMergePatch (RFC 7386)', () => {
  it('follows the RFC examples', () => {
    expect(applyMergePatch({ a: 'b' }, { a: 'c' })).toEqual({ a: 'c' });
    expect(applyMergePatch({ a: 'b' }, { b: 'c' })).toEqual({ a: 'b', b: 'c' });
    expect(applyMergePatch({ a: 'b' }, { a: null })).toEqual({});
    expect(applyMergePatch({ a: ['b'] }, { a: 'c' })).toEqual({ a: 'c' });
    expect(applyMergePatch({ a: { b: 'c' } }, { a: { b: 'd', c: null } })).toEqual({ a: { b: 'd' } });
    expect(applyMergePatch({ a: [{ b: 'c' }] }, { a: [1] })).toEqual({ a: [1] });
    expect(applyMergePatch({ e: null }, { a: 1 })).toEqual({ e: null, a: 1 });
  });

  it('does not mutate the target', () => {
    const t = { waypoints: [{ aglM: 120 }] };
    applyMergePatch(t, { waypoints: [{ aglM: 345 }] });
    expect(t.waypoints[0].aglM).toBe(120);
  });

  it('ignores keys that would reach a prototype', () => {
    const patch = JSON.parse(
      '{"__proto__": {"polluted": true}, "constructor": {"prototype": {"polluted": true}}, "a": {"__proto__": {"x": 1}, "b": 2}}',
    );
    const out = applyMergePatch<Record<string, unknown>>({ a: { c: 3 } }, patch);
    expect(out).toEqual({ a: { c: 3, b: 2 } });
    expect(Object.getPrototypeOf(out)).toBe(Object.prototype);
    expect(Object.getPrototypeOf(out['a'])).toBe(Object.prototype);
    expect(({} as Record<string, unknown>)['polluted']).toBeUndefined();
  });
});
