import { Component, computed, inject, output } from '@angular/core';
import { MissionStore, RfFormat } from '@rfmap/ui-kit';
import { AltitudeView } from '../core/fpv-altitude';
import { ChaseView } from '../core/fpv-chase';

/**
 * The altitude check on the phone sheet, in big numbers: the worst place of the plan, the height it
 * needs there, and the one fix the climb assistant recommends. The profile and the other
 * strategies are a tap away.
 */
@Component({
  selector: 'fpv-phone-altitude',
  template: `
    @if (worst(); as w) {
      <div class="rf-eyebrow">Altitude · worst spot at {{ km(w.s) }}</div>
      <div class="nums">
        <div class="num" [class.bad]="w.margin < 0">
          <span class="v rf-mono">{{ signed(w.margin) }}</span>
          <span class="l">{{ w.margin < 0 ? 'under the floor' : 'over the floor' }}</span>
        </div>
        <div class="num">
          <span class="v rf-mono">{{ m(w.needAgl) }}</span>
          <span class="l">AGL needed there</span>
        </div>
        <div class="num">
          <span class="v rf-mono">{{ m(w.planAgl) }}</span>
          <span class="l">AGL planned</span>
        </div>
      </div>
      @if (fix(); as f) {
        <button
          type="button"
          class="rf-btn rf-btn--primary fix"
          (click)="preview()"
          [disabled]="!!fixHint()"
          [title]="fixHint()"
        >
          Preview fix · {{ f.title }}
        </button>
      } @else if (overCeiling() > 0) {
        <div class="warn">Up to {{ m(overCeiling()) }} over the ceiling.</div>
      } @else if (w.margin >= 0 && !view.stale()) {
        <div class="ok">The plan stays inside the safe corridor.</div>
      }
      @if (view.stale()) {
        <div class="warn" role="status">
          The mission changed since: recalculate to check the heights again.
        </div>
      }
      <div class="more">
        <button type="button" class="rf-btn rf-btn--sm rf-btn--secondary" (click)="profile.emit()">
          Profile
        </button>
        @if (view.climb()) {
          <button
            type="button"
            class="rf-btn rf-btn--sm rf-btn--secondary"
            (click)="assistant.emit()"
          >
            Climb options
          </button>
        }
        @if (chase.track() && !chase.active()) {
          <button type="button" class="rf-btn rf-btn--sm rf-btn--secondary" (click)="chase.start()">
            ▶ Chase in 3D
          </button>
        }
      </div>
    }
  `,
  styles: `
    :host {
      display: flex;
      flex-direction: column;
      gap: 10px;
      flex: none;
    }
    :host:empty {
      display: none;
    }
    .nums {
      display: grid;
      grid-template-columns: repeat(3, minmax(0, 1fr));
      gap: 8px;
    }
    .num {
      padding: 10px;
      border-radius: 10px;
      background: var(--rf-surface-3);
      display: flex;
      flex-direction: column;
      gap: 2px;
    }
    .num .v {
      font-size: 22px;
      font-weight: 600;
      color: var(--rf-text);
      line-height: 1.1;
    }
    .num .l {
      font-size: 11px;
      color: var(--rf-text-2);
    }
    .num.bad {
      background: var(--rf-blocked-bg);
      border: 1px solid var(--rf-blocked-border);
    }
    .num.bad .v {
      color: var(--rf-blocked-text);
    }
    .fix {
      height: 44px;
    }
    .warn {
      font-size: 13px;
      color: var(--rf-warn-text);
    }
    .ok {
      font-size: 13px;
      color: var(--rf-ok-text);
    }
    .more {
      display: flex;
      flex-wrap: wrap;
      gap: 8px;
    }
  `,
})
export class PhoneAltitudeComponent {
  protected readonly view = inject(AltitudeView);
  protected readonly chase = inject(ChaseView);
  private readonly store = inject(MissionStore);

  /** Pull the sheet up to the profile. */
  readonly profile = output<void>();
  /** Open the climb assistant's strategies full screen. */
  readonly assistant = output<void>();

  /** The module's lowest margin over the floor, and the heights at that place. */
  protected readonly worst = computed(() => {
    const sum = this.view.result()?.summary;
    const c = this.view.corridor();
    if (!sum || !c) return null;
    const s = sum.lowestMarginAtM;
    const ground = c.valueAt('groundMslM', s);
    return {
      s,
      margin: sum.lowestMarginM,
      needAgl: c.valueAt('floorMslM', s) - ground,
      planAgl: c.valueAt('plannedMslM', s) - ground,
    };
  });

  /** How far the plan goes over the ceiling at worst, where the floor is under it; 0 inside. */
  protected readonly overCeiling = computed(() => {
    const c = this.view.corridor();
    if (!c) return 0;
    let over = 0;
    for (let i = 0; i < c.c.s.length; i++) {
      if (c.state(i) === 'high') over = Math.max(over, c.c.plannedMslM[i] - c.c.ceilingMslM[i]);
    }
    return over;
  });

  /** The recommended strategy, offered only where the plan needs it and the drone can fly it. */
  protected readonly fix = computed(() => {
    const cl = this.view.climb();
    const w = this.worst();
    if (!cl || !w) return null;
    const needed = w.margin < 0 || cl.plan.overCeilingM > 0 || this.overCeiling() > 0;
    const rec = cl.strategies.find((s) => s.id === cl.recommended);
    return needed && rec?.withinClimbRate ? rec : null;
  });

  protected readonly fixHint = computed(() => {
    if (this.view.stale()) return 'Recalculate first: the mission changed since';
    if (this.store.busy()) return 'Calculating…';
    return '';
  });

  protected preview(): void {
    const f = this.fix();
    if (!f) return;
    this.view.ghost.set(f.id);
    this.store.previewFix(f.fix);
  }

  protected km(s: number): string {
    return RfFormat.format(s, 'km1');
  }

  protected m(v: number): string {
    return RfFormat.format(v, 'm');
  }

  protected signed(v: number): string {
    return RfFormat.format(v, 'm+');
  }
}
