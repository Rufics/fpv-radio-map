import { Mission } from '@rfmap/ui-kit';
import { FpvDataService } from './fpv-data.service';

/**
 * "Use example route": the reference route near Dnipro. Radios and antennas come from the
 * library (the drone's default radios when the library has them).
 */
export function exampleMissionFor(data: FpvDataService): Mission {
  const drone = data.drones()[0];
  const ground = data.antennas().find((a) => a.id.startsWith('ground')) ?? data.antennas()[0];
  const air = data.antennas().find((a) => a.id.startsWith('air')) ?? data.antennas()[0];
  const control = drone?.defaultRadios?.control ?? data.radios().find((r) => r.kind === 'control')?.id ?? 'elrs-433-50hz';
  const video = drone?.defaultRadios?.video ?? data.radios().find((r) => r.kind !== 'control')?.id ?? 'digital-915-20mhz';
  const setup = (radio: string) => ({
    radio,
    txPowerDbm: Math.max(...(data.radio(radio)?.txPowerDbm ?? [30])),
    groundAntenna: ground?.id ?? 'ground-dipole-5dbi',
    airAntenna: air?.id ?? 'air-dipole-2dbi',
    requiredMarginDb: 10,
  });
  return {
    schema: 'mission/v1',
    name: 'North ridge recon',
    time: { mode: 'now' },
    altitudeRef: 'launch',
    pilot: { lat: 48.53, lon: 35.2, aglM: 2, label: 'A' },
    waypoints: [
      { lat: 48.5573394, lon: 35.1717531, altM: 120 },
      { lat: 48.5717285, lon: 35.2108642, altM: 120 },
      { lat: 48.5572773, lon: 35.2586965, altM: 120, label: 'B' },
    ],
    returnToPilot: true,
    links: { control: setup(control), video: setup(video) },
    evaluate: 'both',
    drone: drone?.id,
    battery: data.batteries()[0]?.id,
    modules: { 'fpv.battery': {}, 'rf.weather': {}, 'fpv.example': {} },
    coverage: { radiusM: 8000, cellM: 100, droneAglM: 120 },
  };
}
