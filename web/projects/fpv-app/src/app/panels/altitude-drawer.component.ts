import { Component, computed, inject } from '@angular/core';
import { MissionStore, RfFormat } from '@rfmap/ui-kit';
import { AltitudeView, CorridorState } from '../core/fpv-altitude';
import { AltitudeChart, PROFILE_H, PROFILE_W } from './altitude-chart';

const STATE_TEXT: Record<CorridorState, string> = {
  inside: 'inside the corridor',
  low: 'under the floor',
  ground: 'below the ground',
  high: 'over the ceiling',
  takeoff: 'climb-out zone',
};

/**
 * The altitude profile under the map, linked with the 3D view: ground, the plan coloured by where
 * it is in the safe corridor, the corridor itself, waypoints, and a cursor that moves the 3D
 * cursor and reads out both heights at that point.
 */
@Component({
  selector: 'fpv-altitude-drawer',
  template: `
    <div class="head">
      <h2 id="alt-title">Altitude profile</h2>
      @if (chart()) {
        <div class="keys" aria-hidden="true">
          <span><i class="k-ground"></i>Ground</span>
          <span><i class="k-plan"></i>Plan</span>
          @if (view.corridorShown()) {
            <span><i class="k-band"></i>Safe corridor</span>
            <span><i class="k-floor"></i>Floor</span>
            <span><i class="k-ceiling"></i>Ceiling</span>
          }
        </div>
      }
      <div class="tools">
        <label class="check"
          ><input
            type="checkbox"
            [checked]="view.corridorShown()"
            (change)="view.corridorShown.set(!view.corridorShown())"
          />
          Safe corridor</label
        >
        <label
          class="check"
          title="The first Fresnel zone from the pilot to the cursor, drawn in 3D"
          ><input
            type="checkbox"
            [checked]="view.fresnelShown()"
            (change)="view.fresnelShown.set(!view.fresnelShown())"
          />
          Fresnel zone (3D)</label
        >
        <button
          type="button"
          class="rf-icon-btn"
          aria-label="Close altitude profile"
          (click)="view.profileOpen.set(false)"
        >
          ✕
        </button>
      </div>
    </div>
    @if (chart(); as ch) {
      <div class="body">
        <div class="plot">
          <div class="yaxis rf-mono" aria-hidden="true">
            <span>{{ fmt(ch.max) }}</span
            ><span>{{ fmt(ch.min) }}</span>
          </div>
          <svg
            [attr.viewBox]="'0 0 ' + w + ' ' + h"
            preserveAspectRatio="none"
            role="img"
            aria-labelledby="alt-title"
            [attr.aria-describedby]="'alt-summary'"
            (pointermove)="hover($event)"
            (pointerdown)="hover($event)"
            (pointerleave)="view.cursorS.set(null)"
          >
            @for (z of ch.takeoffZones(); track $index) {
              <rect class="takeoff" [attr.x]="z.x" y="0" [attr.width]="z.w" [attr.height]="h">
                <title>Climb-out zone: the floor rises from the ground</title>
              </rect>
            }
            @if (view.corridorShown()) {
              <path class="band" [attr.d]="ch.band()" />
            }
            <path class="ground" [attr.d]="ch.groundArea()" />
            @if (view.corridorShown()) {
              <path
                class="floor"
                [attr.d]="ch.line('floorMslM')"
                vector-effect="non-scaling-stroke"
              />
              <path
                class="ceiling"
                [attr.d]="ch.line('ceilingMslM')"
                vector-effect="non-scaling-stroke"
              />
            }
            @for (p of ch.planPieces(); track $index) {
              <path
                [attr.class]="'plan is-' + p.state"
                [attr.d]="p.d"
                vector-effect="non-scaling-stroke"
              />
            }
            @for (wp of waypoints(); track wp.index) {
              <line
                class="wp"
                [attr.x1]="ch.x(wp.s)"
                [attr.x2]="ch.x(wp.s)"
                y1="0"
                [attr.y2]="h"
                vector-effect="non-scaling-stroke"
              >
                <title>{{ wp.title }}</title>
              </line>
            }
            @if (cursor(); as c) {
              <line
                class="cursor"
                [attr.x1]="c.x"
                [attr.x2]="c.x"
                y1="0"
                [attr.y2]="h"
                vector-effect="non-scaling-stroke"
              />
            }
          </svg>
          <div class="xaxis" aria-hidden="true">
            @for (t of ch.ticks(); track t.x) {
              <span class="rf-mono" [style.left.%]="t.x / 10">{{ t.label }}</span>
            }
            @for (wp of waypoints(); track wp.index) {
              <span class="wpl" [style.left.%]="ch.x(wp.s) / 10">{{ wp.label }}</span>
            }
          </div>
        </div>
        <div class="side">
          @if (cursor(); as c) {
            <dl class="rf-breakdown" aria-live="polite">
              <dt>At</dt>
              <dd class="rf-mono">{{ km(c.s) }}</dd>
              <dt>Plan</dt>
              <dd class="rf-mono">{{ fmt(c.planned) }} MSL</dd>
              <dt>Above ground</dt>
              <dd class="rf-mono">{{ fmt(c.planned - c.ground) }}</dd>
              <dt>Above launch</dt>
              <dd class="rf-mono">{{ fmt(c.planned - launch()) }}</dd>
              <dt>Floor · ceiling</dt>
              <dd class="rf-mono">{{ fmt(c.floor) }} · {{ fmt(c.ceiling) }}</dd>
              <dt>Margin</dt>
              <dd class="rf-mono" [class]="'is-' + c.state">
                {{ fmtSigned(c.margin) }} · {{ stateText(c.state) }}
              </dd>
            </dl>
          } @else {
            <p id="alt-summary" class="summary">{{ summary() }}</p>
            <p class="hint">
              Hover the profile to read both heights; the 3D view follows the cursor.
            </p>
          }
          @if (view.stale()) {
            <p class="note">The mission changed since this profile was calculated.</p>
          }
        </div>
      </div>
    } @else {
      <div class="empty">
        @if (!view.available()) {
          <p>
            The altitude module (fpv.altitude) is not installed on this server, so there is no
            altitude profile.
          </p>
        } @else if (!view.enabled()) {
          <p>
            Turn on the altitude corridor to see the ground, the plan and the safe corridor along
            the route.
          </p>
          <button
            type="button"
            class="rf-btn rf-btn--sm rf-btn--primary"
            [disabled]="!store.canEdit() || store.busy()"
            (click)="view.enable()"
          >
            Turn on and calculate
          </button>
        } @else if (problem(); as p) {
          <p>{{ p }}</p>
        } @else {
          <p>Calculate the mission to see its altitude profile.</p>
          <button
            type="button"
            class="rf-btn rf-btn--sm rf-btn--primary"
            [disabled]="store.busy()"
            (click)="store.calculate()"
          >
            Calculate
          </button>
        }
      </div>
    }
  `,
  styles: `
    :host {
      display: flex;
      flex-direction: column;
      gap: 8px;
      padding: 12px 16px;
      background: var(--rf-surface-1);
      border-top: 1px solid var(--rf-border);
    }
    .head {
      display: flex;
      align-items: center;
      gap: 14px;
      flex-wrap: wrap;
    }
    h2 {
      margin: 0;
      font-size: 16px;
    }
    .keys {
      display: flex;
      gap: 12px;
      font-size: 11px;
      color: var(--rf-text-2);
    }
    .keys span {
      display: inline-flex;
      align-items: center;
      gap: 5px;
    }
    .keys i {
      width: 14px;
      height: 3px;
      border-radius: 2px;
    }
    .k-ground {
      background: #3a4654;
      height: 10px !important;
    }
    .k-plan {
      background: #35d0ba;
    }
    .k-band {
      background: rgba(53, 208, 186, 0.25);
      height: 10px !important;
    }
    .k-floor {
      background: #ffb547;
    }
    .k-ceiling {
      background: #6ea8fe;
    }
    .tools {
      margin-left: auto;
      display: flex;
      align-items: center;
      gap: 12px;
      font-size: 12px;
    }
    .check {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      color: var(--rf-text-2);
      cursor: pointer;
    }
    .body {
      display: grid;
      grid-template-columns: minmax(0, 1fr) 290px;
      gap: 16px;
    }
    .side dt {
      white-space: nowrap;
    }
    .plot {
      display: grid;
      grid-template-columns: 48px minmax(0, 1fr);
      grid-template-rows: ${PROFILE_H}px 18px;
    }
    .yaxis {
      display: flex;
      flex-direction: column;
      justify-content: space-between;
      font-size: 10px;
      color: var(--rf-text-3);
      text-align: right;
      padding-right: 6px;
    }
    svg {
      display: block;
      width: 100%;
      height: ${PROFILE_H}px;
      background: var(--rf-surface-2);
      border-radius: 6px;
      cursor: crosshair;
      touch-action: none;
    }
    .xaxis {
      grid-column: 2;
      position: relative;
      font-size: 10px;
      color: var(--rf-text-3);
    }
    .xaxis span {
      position: absolute;
      top: 3px;
      transform: translateX(-50%);
      white-space: nowrap;
    }
    .xaxis .wpl {
      color: var(--rf-text);
      font-weight: 600;
      top: -16px;
    }
    .ground {
      fill: #3a4654;
    }
    .band {
      fill: rgba(53, 208, 186, 0.14);
    }
    .takeoff {
      fill: rgba(138, 150, 165, 0.1);
    }
    .floor {
      fill: none;
      stroke: #ffb547;
      stroke-width: 1.5;
    }
    .ceiling {
      fill: none;
      stroke: #6ea8fe;
      stroke-width: 1.5;
      stroke-dasharray: 6 4;
    }
    .plan {
      fill: none;
      stroke-width: 3;
      stroke-linejoin: round;
    }
    .plan.is-inside {
      stroke: #35d0ba;
    }
    .plan.is-low {
      stroke: #ffb547;
    }
    .plan.is-ground {
      stroke: #ff6b5b;
    }
    .plan.is-high {
      stroke: #b98cff;
    }
    .plan.is-takeoff {
      stroke: #8a96a5;
    }
    .wp {
      stroke: var(--rf-text-3);
      stroke-width: 1;
      stroke-dasharray: 2 3;
    }
    .cursor {
      stroke: var(--rf-text);
      stroke-width: 1.5;
    }
    .side {
      display: flex;
      flex-direction: column;
      gap: 8px;
      font-size: 12px;
    }
    .summary {
      margin: 0;
      line-height: 1.5;
    }
    .hint,
    .note {
      margin: 0;
      color: var(--rf-text-3);
      line-height: 1.45;
    }
    .note {
      padding: 6px 8px;
      border-radius: 6px;
      border: 1px solid var(--rf-warn-border);
    }
    dd.is-low,
    dd.is-ground {
      color: var(--rf-warn-text);
    }
    dd.is-high {
      color: #b98cff;
    }
    dd.is-inside {
      color: var(--rf-ok-text);
    }
    .empty {
      display: flex;
      align-items: center;
      gap: 12px;
      padding: 16px 0;
      color: var(--rf-text-2);
      font-size: 13px;
    }
    .empty p {
      margin: 0;
    }
  `,
  host: { role: 'region', 'aria-labelledby': 'alt-title' },
})
export class AltitudeDrawerComponent {
  protected readonly view = inject(AltitudeView);
  protected readonly store = inject(MissionStore);
  protected readonly w = PROFILE_W;
  protected readonly h = PROFILE_H;

  protected readonly chart = computed(() => {
    const c = this.view.corridor();
    return c ? new AltitudeChart(c, this.view.corridorShown()) : null;
  });
  protected readonly launch = computed(() => this.view.result()?.pilot?.groundMslM ?? 0);

  protected readonly waypoints = computed(() =>
    (this.view.result()?.waypoints ?? []).map((w, i, all) => {
      const label = w.label ?? (i === all.length - 1 ? 'B' : `${i + 1}`);
      const title = `${label}: ${this.fmt(w.aboveLaunchM)} above launch · ${this.fmt(w.aglM)} above ground · ${this.fmt(w.mslM)} MSL`;
      return { index: w.index, s: w.s, label, title };
    }),
  );

  protected readonly cursor = computed(() => {
    const s = this.view.cursorS();
    const c = this.view.corridor();
    const ch = this.chart();
    if (s === null || !c || !ch) return null;
    const planned = c.valueAt('plannedMslM', s);
    const floor = c.valueAt('floorMslM', s);
    const ceiling = c.valueAt('ceilingMslM', s);
    const state = c.state(c.indexAt(s));
    // Over the ceiling the margin that matters is to the ceiling; everywhere else, to the floor.
    const margin = state === 'high' ? ceiling - planned : planned - floor;
    return {
      s,
      x: ch.x(s),
      planned,
      ground: c.valueAt('groundMslM', s),
      floor,
      ceiling,
      state,
      margin,
    };
  });

  protected readonly summary = computed(() => {
    const sum = this.view.result()?.summary;
    if (!sum) return '';
    const margin =
      sum.lowestMarginM >= 0
        ? `stays at least ${this.fmt(sum.lowestMarginM)} over the floor`
        : `drops ${this.fmt(-sum.lowestMarginM)} under the floor at ${this.km(sum.lowestMarginAtM)}`;
    return `Highest ground ${this.fmt(sum.highestGroundMslM)} at ${this.km(sum.highestGroundAtM)}. The plan peaks at ${this.fmt(sum.highestPlannedMslM)} and ${margin}. Heights are above sea level.`;
  });

  /** The module's own warning when it could not draw the corridor. */
  protected readonly problem = computed(() => {
    const r = this.store.result();
    if (!r || this.view.result()?.corridor) return null;
    return (r.reasons ?? []).find((x) => x.module === 'fpv.altitude')?.text ?? null;
  });

  protected hover(e: PointerEvent): void {
    const ch = this.chart();
    const box = (e.currentTarget as SVGElement).getBoundingClientRect();
    if (!ch || box.width <= 0) return;
    this.view.cursorS.set(ch.sAt((e.clientX - box.left) / box.width));
  }

  protected fmt(v: number): string {
    return RfFormat.format(v, 'm');
  }

  protected fmtSigned(v: number): string {
    return RfFormat.format(v, 'm+');
  }

  protected km(s: number): string {
    return `km ${(s / 1000).toFixed(2)}`;
  }

  protected stateText(s: CorridorState): string {
    return STATE_TEXT[s];
  }
}
