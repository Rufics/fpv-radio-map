import { describe, expect, it } from 'vitest';
import { Mission, MissionAltitude, applyMergePatch } from '@rfmap/ui-kit';
import { fpvAltitudeAnalyzeMock } from './fpv-altitude-mock';
import { exampleMission } from './reference-case';

const KINDS = ['energy', 'over-launch', 'gradual'];

describe('fpvClimbMock', () => {
  const m = { ...exampleMission(), modules: { 'fpv.altitude': {} } };
  const { data, reasons } = fpvAltitudeAnalyzeMock(m);
  const climb = data.climb!;
  const c = data.corridor!;

  it('offers the three strategies, the energy-optimal one first and recommended', () => {
    expect(climb.strategies.map((s) => s.id)).toEqual(KINDS);
    expect(climb.recommended).toBe('energy');
    for (const s of climb.strategies) {
      expect(s.mslM).toHaveLength(c.s.length);
      expect(s.fix.id).toBe(`fpv.altitude/climb-${s.id}`);
      expect(s.fix.preview?.[0].label).toBe('Total climb');
    }
  });

  it('climbs less than the plan, and than any strategy that keeps under the ceiling as well', () => {
    const [energy, ...rest] = climb.strategies;
    expect(energy.riseM).toBeLessThan(climb.plan.riseM);
    // Where the floor is over the ceiling, every profile has to be over it.
    const under = (p: number[]) =>
      p.every((v, i) => c.floorMslM[i] > c.ceilingMslM[i] || v <= c.ceilingMslM[i] + 0.1);
    expect(under(energy.mslM)).toBe(true);
    for (const s of rest.filter((x) => under(x.mslM)))
      expect(energy.riseM).toBeLessThanOrEqual(s.riseM + 1e-6);
  });

  it('keeps each fitted profile over the floor past the take-off zone', () => {
    for (const s of climb.strategies) expect(s.minClearanceM).toBeGreaterThan(-0.05);
  });

  it('estimates energy for the mission drone', () => {
    expect(climb.energyEstimated).toBe(!!m.drone);
    if (m.drone) {
      expect(climb.plan.energyWh).toBeGreaterThan(0);
      expect(climb.drone?.id).toBe(m.drone);
    }
    const none = fpvAltitudeAnalyzeMock({ ...m, drone: undefined }).data.climb!;
    expect(none.energyEstimated).toBe(false);
    expect(none.plan.energyWh).toBeUndefined();
    expect(none.notes[0]).toContain('No drone picked');
    expect(none.notes.at(-1)).toContain('follow the terrain between waypoints');
  });

  it('writes the strategy into the waypoints through the fix patch', () => {
    const energy = climb.strategies[0];
    const after = applyMergePatch(m, energy.fix.patch!) as Mission;
    expect(after.waypoints).toHaveLength(m.waypoints.length + energy.addedWaypoints);
    expect(after.waypoints.every((w) => w.heightSource === 'manual')).toBe(true);
    const field = MissionAltitude.field(m);
    expect(after.waypoints.every((w) => typeof w[field] === 'number')).toBe(true);
    const again = fpvAltitudeAnalyzeMock(after).data.climb!;
    expect(again.plan.riseM).toBeCloseTo(energy.riseM, 0);
  });

  it('warns about a leg the drone cannot climb, and points at the energy fix', () => {
    const steep = {
      ...m,
      modules: { 'fpv.altitude': { climbRateMps: 0.5 } },
      waypoints: m.waypoints.map((w, i) => (i === 0 ? { ...w, aglM: 400 } : w)),
    };
    const r = fpvAltitudeAnalyzeMock(steep).reasons;
    const warn = r.find((x) => x.severity === 'warn');
    expect(warn?.text).toMatch(/^Leg Pilot → .*the drone climbs 0\.5 m\/s\.$/);
    expect(warn?.leg).toBe(0);
    expect(warn?.fix?.id).toBe('fpv.altitude/climb-energy');
    expect(r.some((x) => x.severity === 'info' && x.text?.startsWith('Climb assistant'))).toBe(
      true,
    );
  });

  it('gives no advice on a plan as good as the energy-optimal one', () => {
    const energy = climb.strategies[0];
    const after = applyMergePatch(m, energy.fix.patch!) as Mission;
    const r = fpvAltitudeAnalyzeMock(after).reasons;
    expect(r.filter((x) => x.severity === 'info')).toHaveLength(0);
    expect(reasons.every((x) => x.module === 'fpv.altitude')).toBe(true);
  });

  it('advises a plan under the floor into the corridor', () => {
    const low = { ...m, waypoints: m.waypoints.map((w) => ({ ...w, aglM: 5 })) };
    const advice = fpvAltitudeAnalyzeMock(low).reasons.find((x) => x.severity === 'info');
    expect(advice?.text).toMatch(
      /^Climb assistant: the energy-optimal profile keeps the whole route inside the corridor, climbing \d+ m in all against the plan's \d+ m( \(\d+\.\d Wh (more|less)\))?\.$/,
    );
  });

  it('flies the drone at the speed option', () => {
    const at = (speedMps: number) =>
      fpvAltitudeAnalyzeMock({ ...m, modules: { 'fpv.altitude': { speedMps } } }).data.climb!;
    expect(at(20).plan.timeS).toBeLessThan(at(0).plan.timeS);
    expect(at(20).airspeedMps).toBe(20);
    expect(at(20).notes[0]).toContain('cruise at 20.0 m/s');
  });

  it('names the target, not the pilot, at the end of a one-way route', () => {
    const last = m.waypoints.length - 1;
    const oneWay = {
      ...m,
      returnToPilot: false,
      modules: { 'fpv.altitude': { climbRateMps: 0.5 } },
      waypoints: m.waypoints.map((w, i) => (i === last ? { ...w, aglM: 400 } : w)),
    };
    const warn = fpvAltitudeAnalyzeMock(oneWay).reasons.find((x) => x.leg === last);
    expect(warn?.text).toMatch(new RegExp(`→ ${m.waypoints[last].label}:`));
  });

  it('coarsens a grid too fine for the corridor, and says so', () => {
    const fine = fpvAltitudeAnalyzeMock({
      ...m,
      modules: { 'fpv.altitude': { heightStepM: 0.1, ceilingAglM: 2000 } },
    }).data.climb!;
    expect(fine.heightStepM).toBeGreaterThan(1);
    expect(fine.notes.some((n) => n.startsWith('Heights searched every'))).toBe(true);
  });

  it('stays out when the assistant is off', () => {
    const off = fpvAltitudeAnalyzeMock({
      ...m,
      modules: { 'fpv.altitude': { climbAssist: false } },
    });
    expect(off.data.climb).toBeUndefined();
    expect(off.reasons).toEqual([]);
  });

  it('flies a launch-referenced mission and a one-way route', () => {
    const launch = {
      ...m,
      altitudeRef: 'launch' as const,
      returnToPilot: false,
      waypoints: m.waypoints.map(({ aglM: _agl, ...w }) => ({ ...w, altM: 150 })),
    };
    const d = fpvAltitudeAnalyzeMock(launch).data.climb!;
    expect(d.strategies.map((s) => s.id)).toEqual(KINDS);
    expect(d.strategies[2].summary).toContain('then fly level');
    const patch = d.strategies[0].fix.patch as { waypoints: Record<string, unknown>[] };
    expect(patch.waypoints.every((w) => typeof w['altM'] === 'number')).toBe(true);
  });
});
