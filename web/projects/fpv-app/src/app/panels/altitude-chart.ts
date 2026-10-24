import { Corridor, CorridorState } from '../core/fpv-altitude';

/** Chart size in SVG units; the SVG stretches it to the drawer's width. */
export const PROFILE_W = 1000;
export const PROFILE_H = 160;

export interface PlanPieceVm {
  state: CorridorState;
  d: string;
}

export interface TickVm {
  x: number;
  label: string;
}

/** Turns a corridor into SVG geometry for the altitude profile. */
export class AltitudeChart {
  readonly min: number;
  readonly max: number;

  constructor(
    private readonly corridor: Corridor,
    withCorridor: boolean,
  ) {
    const c = corridor.c;
    const tops = [
      ...c.plannedMslM,
      ...c.groundMslM,
      ...(withCorridor ? [...c.ceilingMslM, ...c.floorMslM] : []),
    ];
    const lo = Math.min(...c.groundMslM, ...c.plannedMslM);
    const hi = Math.max(...tops);
    const pad = Math.max(10, (hi - lo) * 0.06);
    this.min = Math.floor((lo - pad) / 10) * 10;
    this.max = Math.ceil((hi + pad) / 10) * 10;
  }

  x(s: number): number {
    const len = this.corridor.length;
    return len > 0 ? (s / len) * PROFILE_W : 0;
  }

  y(mslM: number): number {
    const span = this.max - this.min;
    return span > 0 ? PROFILE_H - ((mslM - this.min) / span) * PROFILE_H : PROFILE_H;
  }

  /** Distance along the route at a fraction 0..1 of the chart width. */
  sAt(fraction: number): number {
    return Math.min(1, Math.max(0, fraction)) * this.corridor.length;
  }

  line(
    key: 'groundMslM' | 'plannedMslM' | 'floorMslM' | 'ceilingMslM' | 'terrainFloorMslM',
  ): string {
    const c = this.corridor.c;
    return c.s
      .map((s, i) => `${i ? 'L' : 'M'}${this.x(s).toFixed(1)},${this.y(c[key][i]).toFixed(1)}`)
      .join('');
  }

  /** The ground as a filled area down to the bottom of the chart. */
  groundArea(): string {
    const c = this.corridor.c;
    return `${this.line('groundMslM')}L${this.x(c.s.at(-1) ?? 0).toFixed(1)},${PROFILE_H}L0,${PROFILE_H}Z`;
  }

  /** Between floor and ceiling. */
  band(): string {
    const c = this.corridor.c;
    const top = c.s
      .map(
        (s, i) => `${i ? 'L' : 'M'}${this.x(s).toFixed(1)},${this.y(c.ceilingMslM[i]).toFixed(1)}`,
      )
      .join('');
    const bottom = [...c.s.keys()]
      .reverse()
      .map((i) => `L${this.x(c.s[i]).toFixed(1)},${this.y(c.floorMslM[i]).toFixed(1)}`)
      .join('');
    return `${top}${bottom}Z`;
  }

  /** The plan cut into pieces of one state each; a piece takes the state of its first sample. */
  planPieces(): PlanPieceVm[] {
    const c = this.corridor.c;
    const out: PlanPieceVm[] = [];
    let cur = null as PlanPieceVm | null;
    for (let i = 0; i + 1 < c.s.length; i++) {
      const state = this.corridor.state(i);
      const a = `${this.x(c.s[i]).toFixed(1)},${this.y(c.plannedMslM[i]).toFixed(1)}`;
      const b = `L${this.x(c.s[i + 1]).toFixed(1)},${this.y(c.plannedMslM[i + 1]).toFixed(1)}`;
      if (cur?.state === state) cur.d += b;
      else {
        cur = { state, d: `M${a}${b}` };
        out.push(cur);
      }
    }
    return out;
  }

  /** Width of the take-off zone at the start, and of the landing zone at the end on a return. */
  takeoffZones(): { x: number; w: number }[] {
    const c = this.corridor.c;
    const zones: { x: number; w: number }[] = [];
    let start: number | null = null;
    for (let i = 0; i < c.s.length; i++) {
      const near = this.corridor.nearPilot(i);
      if (near && start === null) start = c.s[i];
      // A zone ends at the first sample out of it, or at the end of the route.
      if ((!near || i === c.s.length - 1) && start !== null) {
        zones.push({ x: this.x(start), w: this.x(c.s[i]) - this.x(start) });
        start = null;
      }
    }
    return zones.filter((z) => z.w > 0);
  }

  /** Kilometre ticks, at most about ten. */
  ticks(): TickVm[] {
    const km = this.corridor.length / 1000;
    const step = [0.5, 1, 2, 5, 10, 20, 50].find((s) => km / s <= 10) ?? 100;
    const out: TickVm[] = [];
    for (let k = 0; k <= km + 1e-9; k += step)
      out.push({ x: this.x(k * 1000), label: `${Number(k.toFixed(1))} km` });
    return out;
  }
}
