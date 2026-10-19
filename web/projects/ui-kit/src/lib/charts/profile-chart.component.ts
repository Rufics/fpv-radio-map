import { Component, DestroyRef, ElementRef, afterNextRender, computed, inject, input, signal } from '@angular/core';
import { Diffraction, Profile } from '../models/api.models';
import { RfFormat } from '../format/rf-format';
import { ProfileGeometry, computeProfileGeometry, niceStep } from './profile-geometry';

interface Tick {
  pos: number;
  label: string;
}

/** Plot area and the distance/height to pixel scales of one render. */
interface ChartFrame {
  X: (d: number) => number;
  Y: (m: number) => number;
  x0: number;
  x1: number;
}

/**
 * Terrain profile with the direct ray, the first Fresnel ellipse (and 60 % zone),
 * and the part of the terrain inside the zone highlighted. SVG, sized to its host.
 */
@Component({
  selector: 'rf-profile-chart',
  template: `
    @if (view(); as v) {
      <svg [attr.viewBox]="'0 0 ' + v.w + ' ' + v.h" [attr.width]="v.w" [attr.height]="v.h" role="img" [attr.aria-label]="v.aria">
        <g class="grid">
          @for (t of v.yTicks; track t.pos) {
            <line [attr.x1]="v.x0" [attr.x2]="v.x1" [attr.y1]="t.pos" [attr.y2]="t.pos" />
          }
          @for (t of v.xTicks; track t.pos) {
            <line [attr.x1]="t.pos" [attr.x2]="t.pos" [attr.y1]="v.y1" [attr.y2]="v.y0" />
          }
        </g>
        <g class="axis">
          @for (t of v.yTicks; track t.pos) {
            <text [attr.x]="v.x0 - 8" [attr.y]="t.pos + 4" text-anchor="end">{{ t.label }}</text>
          }
          @for (t of v.xTicks; track t.pos; let last = $last) {
            <text [attr.x]="t.pos" [attr.y]="v.y0 + 18" [attr.text-anchor]="last ? 'end' : 'middle'">{{ t.label }}</text>
          }
        </g>
        <path class="terrain" [attr.d]="v.terrain"><title>Terrain (Earth bulge for k = {{ v.k }})</title></path>
        <path class="fresnel" [attr.d]="v.fresnel"><title>First Fresnel zone</title></path>
        <path class="fresnel60" [attr.d]="v.fresnel60" />
        @for (p of v.intrusions; track $index) {
          <path class="intrusion" [attr.d]="p"><title>Terrain inside the first Fresnel zone</title></path>
        }
        <line class="ray" [attr.x1]="v.ax" [attr.y1]="v.ay" [attr.x2]="v.bx" [attr.y2]="v.by"><title>Direct ray</title></line>
        <line class="agl" [attr.x1]="v.bx" [attr.y1]="v.by" [attr.x2]="v.bx" [attr.y2]="v.bGround" />
        <line class="agl" [attr.x1]="v.ax" [attr.y1]="v.ay" [attr.x2]="v.ax" [attr.y2]="v.aGround" />
        <circle class="end-a" [attr.cx]="v.ax" [attr.cy]="v.ay" r="9" />
        <text class="end-label-a" [attr.x]="v.ax" [attr.y]="v.ay + 4" text-anchor="middle">{{ fromLabel() }}</text>
        <circle class="end-b" [attr.cx]="v.bx" [attr.cy]="v.by" r="9" />
        <text class="end-label-b" [attr.x]="v.bx" [attr.y]="v.by + 4" text-anchor="middle">{{ toLabel() }}</text>
        @if (v.obstacle; as o) {
          <line class="obstacle-lead" [attr.x1]="o.x" [attr.x2]="o.x" [attr.y1]="o.y - 4" [attr.y2]="o.boxY + 26" />
          <rect class="obstacle-box" [attr.x]="o.boxX" [attr.y]="o.boxY" [attr.width]="o.boxW" height="26" rx="6" />
          <text class="obstacle-text" [attr.x]="o.boxX + o.boxW / 2" [attr.y]="o.boxY + 18" text-anchor="middle">
            {{ o.text }}<title>{{ o.title }}</title>
          </text>
        }
      </svg>
    } @else {
      <div class="empty">No terrain profile for this link</div>
    }
  `,
  styles: `
    :host { display: block; position: relative; width: 100%; height: 100%; min-height: 120px; background: var(--rf-surface-1); font-family: var(--rf-font-sans); }
    svg { display: block; position: absolute; inset: 0; }
    .grid line { stroke: #1F2933; stroke-width: 1; }
    .axis text { font-family: var(--rf-font-mono); font-size: 11px; fill: #8C98A6; font-variant-numeric: tabular-nums; }
    .terrain { fill: var(--rf-terrain-fill); stroke: var(--rf-terrain-line); stroke-width: 1.5; }
    .fresnel { fill: var(--rf-ok); fill-opacity: 0.13; stroke: var(--rf-ok); stroke-opacity: 0.6; stroke-dasharray: 5 4; }
    .fresnel60 { fill: none; stroke: var(--rf-ok); stroke-opacity: 0.35; stroke-dasharray: 2 4; }
    .intrusion { fill: var(--rf-blocked); fill-opacity: 0.75; }
    .ray { stroke: var(--rf-ok); stroke-width: 2.5; }
    .agl { stroke: var(--rf-warn); stroke-dasharray: 3 3; }
    .end-a { fill: var(--rf-ok); }
    .end-b { fill: var(--rf-warn); }
    .end-label-a { fill: #06201C; font-size: 11px; font-weight: 700; }
    .end-label-b { fill: #2A1A00; font-size: 11px; font-weight: 700; }
    .obstacle-lead { stroke: var(--rf-blocked); stroke-dasharray: 3 3; }
    .obstacle-box { fill: var(--rf-blocked-bg); stroke: var(--rf-blocked); }
    .obstacle-text { fill: var(--rf-blocked-text); font-size: 13px; font-weight: 600; }
    .empty { padding: 16px; color: var(--rf-text-2); }
  `,
})
export class ProfileChartComponent {
  readonly profile = input<Profile | null | undefined>();
  readonly fromAglM = input(2);
  readonly toAglM = input(120);
  /** Antenna MSL of each end (LinkResult.ends); preferred over profile ground + AGL */
  readonly fromAntennaMslM = input<number | undefined>(undefined);
  readonly toAntennaMslM = input<number | undefined>(undefined);
  readonly frequencyHz = input(915e6);
  readonly kFactor = input<number | undefined>(undefined);
  readonly diffraction = input<Diffraction | undefined>(undefined);
  /** Loss shown in the obstacle label */
  readonly lossDb = input<number | undefined>(undefined);
  readonly obstacleName = input('Obstacle');
  readonly fromLabel = input('A');
  readonly toLabel = input('B');

  private readonly host = inject<ElementRef<HTMLElement>>(ElementRef);
  private readonly size = signal({ w: 1000, h: 280 });

  constructor() {
    const destroy = inject(DestroyRef);
    afterNextRender(() => {
      const el = this.host.nativeElement;
      const measure = () => {
        const r = el.getBoundingClientRect();
        if (r.width > 0 && r.height > 0) this.size.set({ w: Math.round(r.width), h: Math.round(r.height) });
      };
      measure();
      const ro = new ResizeObserver(measure);
      ro.observe(el);
      destroy.onDestroy(() => ro.disconnect());
    });
  }

  protected readonly view = computed(() => {
    const profile = this.profile();
    if (!profile || profile.points.length < 2) return null;
    let g;
    try {
      g = computeProfileGeometry({
        profile,
        fromAglM: this.fromAglM(),
        toAglM: this.toAglM(),
        fromAntennaMslM: this.fromAntennaMslM(),
        toAntennaMslM: this.toAntennaMslM(),
        frequencyHz: this.frequencyHz(),
        kFactor: this.kFactor(),
        diffraction: this.diffraction(),
      });
    } catch {
      return null;
    }
    const { w, h } = this.size();
    const narrow = w < 520;
    const x0 = narrow ? 44 : 60;
    const x1 = w - (narrow ? 16 : 40);
    const y1 = narrow ? 36 : 44;
    const y0 = h - 26;
    const step = niceStep(g.maxH - g.minH, narrow ? 3 : 4);
    const lo = Math.floor((g.minH - step * 0.25) / step) * step;
    const hi = Math.ceil((g.maxH + step * 0.1) / step) * step;
    const X = (d: number) => x0 + ((x1 - x0) * d) / g.lengthM;
    const Y = (m: number) => y0 - ((y0 - y1) * (m - lo)) / (hi - lo);

    const yTicks: Tick[] = [];
    for (let m = lo; m <= hi + 1e-6; m += step) {
      yTicks.push({ pos: Y(m), label: m === lo ? `${RfFormat.format(m, 'm', { unit: false })} m` : RfFormat.format(m, 'm', { unit: false }) });
    }
    const xTicks: Tick[] = [0, 0.25, 0.5, 0.75, 1].map((f, i, arr) => ({
      pos: X(g.lengthM * f),
      label: f === 0 ? '0' : RfFormat.format(g.lengthM * f, 'km1', { unit: i === arr.length - 1 }),
    }));

    const s = g.samples;
    const line = (pts: [number, number][]) => pts.map((p, i) => `${i ? 'L' : 'M'}${p[0].toFixed(1)} ${p[1].toFixed(1)}`).join(' ');
    const terrain = `${line([[X(0), y0], ...s.map((p) => [X(p.d), Y(p.surface)] as [number, number]), [X(g.lengthM), y0]])} Z`;
    const zone = (f: number) =>
      `${line([...s.map((p) => [X(p.d), Y(p.ray + f * p.r1)] as [number, number]), ...[...s].reverse().map((p) => [X(p.d), Y(p.ray - f * p.r1)] as [number, number])])} Z`;
    const intrusions = g.intrusions.map((i) => `${line(i.polygon.map(([d, m]) => [X(d), Y(m)] as [number, number]))} Z`);

    const dif = this.diffraction();
    const blocking = g.intrusions.length > 0 || dif?.lineOfSight === false;
    const obstacle = blocking ? this.obstacleView(g, dif, { X, Y, x0, x1 }) : null;
    const first = s[0];
    const last = s.at(-1)!;
    const aria =
      `Terrain profile ${this.fromLabel()} to ${this.toLabel()}, ${RfFormat.format(g.lengthM, 'km3')}. ` +
      (blocking ? `Terrain enters the first Fresnel zone; worst clearance ${RfFormat.format(g.worst.clearanceM, 'm+')}.` : 'First Fresnel zone is clear.');
    return {
      w,
      h,
      x0,
      x1,
      y0,
      y1,
      yTicks,
      xTicks,
      terrain,
      fresnel: zone(1),
      fresnel60: zone(0.6),
      intrusions,
      ax: X(0),
      ay: Y(g.fromMsl),
      aGround: Y(first.surface),
      bx: X(g.lengthM),
      by: Y(g.toMsl),
      bGround: Y(last.surface),
      obstacle,
      k: RfFormat.format(this.kFactor() ?? 4 / 3, 'n2'),
      aria,
    };
  });

  /** Label box over the dominant obstacle (or the worst Fresnel intrusion). */
  private obstacleView(g: ProfileGeometry, dif: Diffraction | undefined, frame: ChartFrame) {
    const { X, Y, x0, x1 } = frame;
    const worstIntrusion = g.intrusions.reduce<(typeof g.intrusions)[number] | null>(
      (m, i) => (!m || i.clearance < m.clearance ? i : m),
      null,
    );
    const d = dif?.dominant?.distanceM ?? dif?.d1M ?? worstIntrusion?.maxD ?? g.worst.d;
    const edge =
      dif?.edgeHeightM ?? dif?.dominant?.groundMslM ?? worstIntrusion?.maxSurface ?? g.worst.surface;
    const loss = this.lossDb() ?? dif?.lossDb;
    const lossText = loss !== undefined ? ` · ${RfFormat.format(-loss, 'db')}` : '';
    const lossTitle = loss !== undefined ? `, loss ${RfFormat.full(loss, 'db')}` : '';
    const text = `${this.obstacleName()} ${RfFormat.format(edge, 'm')}${lossText}`;
    const boxW = Math.max(150, text.length * 7.4 + 24);
    const x = X(d);
    return {
      x,
      y: Y(edge),
      boxX: Math.min(Math.max(x - boxW / 2, x0), x1 - boxW),
      boxY: 8,
      boxW,
      text,
      title: `edge ${RfFormat.full(edge, 'm')} MSL at ${RfFormat.full(d, 'km3')}${lossTitle}`,
    };
  }
}
