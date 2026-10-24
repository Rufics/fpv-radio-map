import { SchemaValidator } from './schema-validator';

describe('SchemaValidator (contract schemas, ajv draft-07 strict)', () => {
  const v = new SchemaValidator();

  it('accepts a valid mission and rejects a broken one', () => {
    const ok = v.validateMission({
      schema: 'mission/v1',
      name: 'North ridge recon',
      time: { mode: 'now' },
      pilot: { lat: 48.53, lon: 35.2, aglM: 2 },
      waypoints: [{ lat: 48.557, lon: 35.258, aglM: 120 }],
      links: {},
      evaluate: 'both',
    });
    expect(ok.issues).toEqual([]);
    const bad = v.validateMission({ schema: 'mission/v1', name: 'x', time: { mode: 'later' }, pilot: { lat: 95, lon: 0, aglM: -1 }, waypoints: [], links: {}, evaluate: 'both' });
    expect(bad.ok).toBe(false);
    expect(bad.issues.map((i) => i.path)).toEqual(expect.arrayContaining(['/time/mode', '/pilot/lat', '/pilot/aglM', '/waypoints']));
  });

  it('accepts a waypoint left to the server but not a manual one without its height', () => {
    const m = { schema: 'mission/v1', name: 'Auto', time: { mode: 'now' }, pilot: { lat: 48.53, lon: 35.2, aglM: 2 }, links: {}, evaluate: 'both' };
    const r = v.validateMission({ ...m, waypoints: [{ lat: 48.557, lon: 35.258 }] });
    expect(r.issues).toEqual([]);
    expect(v.validateMission({ ...m, waypoints: [{ lat: 48.557, lon: 35.258, aglM: 90, heightSource: 'auto' }] }).issues).toEqual([]);
    expect(v.validateMission({ ...m, waypoints: [{ lat: 48.557, lon: 35.258, heightSource: 'manual' }] }).ok).toBe(false);
    const launch = { ...m, altitudeRef: 'launch', waypoints: [{ lat: 48.557, lon: 35.258, aglM: 90, heightSource: 'manual' }] };
    expect(v.validateMission(launch).ok).toBe(false);
  });

  it('validates a battery entry with the shared common definitions', () => {
    const entry = {
      id: 'liion-6s-6000',
      name: '6S Li-ion 6000 mAh',
      version: 1,
      verified: true,
      chemistry: 'li-ion',
      cellsSeries: 6,
      capacityAh: 6,
      nominalCellVoltage: 3.6,
      usableFraction: 0.8,
      source: { kind: 'datasheet', reference: 'Cell datasheet' },
    };
    expect(v.validateLibrary('batteries', entry).ok).toBe(true);
    const res = v.validateLibrary('batteries', { ...entry, id: 'X', extra: 1, source: { kind: 'guess', reference: 'x' } });
    expect(res.ok).toBe(false);
    const msgs = res.issues.map((i) => `${i.path} ${i.message}`);
    expect(msgs.some((m) => m.includes('/id'))).toBe(true);
    expect(msgs.some((m) => m.includes('unknown field “extra”'))).toBe(true);
    expect(msgs.some((m) => m.startsWith('/source/kind'))).toBe(true);
  });
});
