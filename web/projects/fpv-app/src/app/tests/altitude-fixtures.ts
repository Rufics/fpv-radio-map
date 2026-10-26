import { FpvAltitudeCorridor } from '../core/fpv-altitude';

/** Four samples 1 km apart due north of the pilot; the plan is level at 600 m MSL. */
export function corridorOf(over: Partial<FpvAltitudeCorridor> = {}): FpvAltitudeCorridor {
  return {
    stepM: 1000,
    clearanceM: 30,
    takeoffM: 200,
    ceilingAglM: 120,
    linkFloor: true,
    fresnelFraction: 0.6,
    s: [0, 1000, 2000, 3000],
    lat: [48.5, 48.509, 48.518, 48.527],
    lon: [35.2, 35.2, 35.2, 35.2],
    leg: [0, 0, 0, 0],
    groundMslM: [500, 520, 590, 450],
    plannedMslM: [501.5, 600, 600, 600],
    floorMslM: [505, 550, 620, 480],
    terrainFloorMslM: [500, 550, 620, 480],
    linkFloorMslM: [null, 540, null, 470],
    ceilingMslM: [620, 640, 710, 570],
    fresnelRadiusM: [0, 10, 14, 17],
    ...over,
  };
}
