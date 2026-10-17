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
});
