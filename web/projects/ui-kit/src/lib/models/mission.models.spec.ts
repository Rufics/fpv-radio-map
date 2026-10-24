import { Mission, MissionAltitude } from './mission.models';

const base: Mission = {
  schema: 'mission/v1',
  name: 'm',
  time: { mode: 'now' },
  pilot: { lat: 48.53, lon: 35.2, aglM: 2 },
  waypoints: [{ lat: 48.55, lon: 35.17, aglM: 120, label: 'B' }],
  returnToPilot: true,
  links: {},
  evaluate: 'both',
};

describe('MissionAltitude', () => {
  it('treats a mission without altitudeRef as an AGL mission', () => {
    expect(MissionAltitude.ref(base)).toBe('agl');
    expect(MissionAltitude.field(base)).toBe('aglM');
    expect(MissionAltitude.height(base, base.waypoints[0])).toBe(120);
    expect(MissionAltitude.unit(base)).toBe('m AGL');
  });

  it('reads and writes heights above launch in altM', () => {
    const launch = { altitudeRef: 'launch' as const };
    const w = MissionAltitude.withHeight(launch, base.waypoints[0], 80);
    expect(w).toEqual({ lat: 48.55, lon: 35.17, altM: 80, label: 'B' });
    expect(MissionAltitude.height(launch, w)).toBe(80);
    expect(MissionAltitude.unit(launch)).toBe('m above launch');
  });

  it('switches reference keeping the numbers and the pilot', () => {
    const launch = MissionAltitude.withRef(base, 'launch');
    expect(launch.altitudeRef).toBe('launch');
    expect(launch.waypoints[0]).toEqual({ lat: 48.55, lon: 35.17, altM: 120, label: 'B' });
    expect(launch.pilot.aglM).toBe(2);
    expect(MissionAltitude.withRef(launch, 'agl').waypoints[0]).toEqual(base.waypoints[0]);
    expect(MissionAltitude.withRef(base, 'agl')).toBe(base);
  });
});
