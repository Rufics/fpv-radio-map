import { Component, computed, inject } from '@angular/core';
import { PillComponent, RfFormat } from '@rfmap/ui-kit';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { exampleMissionFor } from '../core/example-mission';
import { WhyDrawerComponent } from '../panels/why-drawer.component';
import { PreviewPanelComponent } from '../panels/preview-panel.component';
import { MapStageComponent } from './map-stage.component';

/** Layout C (phone, view-oriented): full map + bottom sheet; "why" opens full screen. */
@Component({
  selector: 'fpv-phone-layout',
  imports: [PillComponent, MapStageComponent, WhyDrawerComponent, PreviewPanelComponent],
  host: { class: 'phone' },
  template: `
    @if (ws.why() && ws.whyLeg(); as leg) {
      <section class="why" aria-labelledby="phone-why-title">
        <div class="why-head">
          <button type="button" class="back" aria-label="Back to mission" (click)="ws.why.set(null)">‹</button>
          <b id="phone-why-title">{{ whyTitle() }}</b>
        </div>
        @if (verdictText(); as t) {
          <div class="reason">{{ t }}</div>
        }
        <fpv-why-drawer [compact]="true" />
        @if (!ws.store.canEdit()) {
          <div class="foot">View-only link. Ask the mission owner to change the route.</div>
        }
      </section>
    } @else {
      <div class="map">
        <fpv-map-stage class="stage" [fitPadding]="padding" [compact]="true" [interactiveEdit]="false" />
        <div class="chips">
          <span class="chip name">{{ ws.mission()?.name ?? 'FPV Radio Map' }}</span>
          @if (ws.mission()) {
            <span class="chip role">{{ ws.store.canEdit() ? 'Can edit' : 'View only' }}</span>
          }
        </div>
      </div>
      <section class="sheet" aria-label="Mission summary">
        <div class="handle" aria-hidden="true"></div>
        @if (!ws.mission()) {
          <h2>Plan on a laptop, check here</h2>
          <div class="rf-body">Open a shared mission link, or look at the example route.</div>
          <button type="button" class="rf-btn rf-btn--primary" (click)="example()" [disabled]="!ws.data.loaded()">Use example route</button>
        } @else if (ws.store.preview()) {
          <fpv-preview-panel />
        } @else {
          @if (ws.pills().length) {
            <div class="pills">
              @for (p of ws.pills(); track p.key) {
                <rf-pill [tone]="p.tone">{{ p.text }}</rf-pill>
              }
            </div>
          }
          <div class="rf-eyebrow">Route · {{ ws.routeLengthText() }}{{ ws.store.result() ? ' · tap a leg' : '' }}</div>
          <div class="legs">
            @for (r of ws.legRows(); track r.index) {
              @if (ws.store.result() && r.mark) {
                <button type="button" class="leg" [class.bad]="r.tone === 'blocked'" (click)="ws.openWhy(r.index)" [attr.aria-label]="r.name + ', ' + r.info + ', ' + r.markLabel + '. Show why'">
                  <span>{{ r.name }} · {{ km(r.lengthM) }}</span><span class="mark" [class]="'mark tone-' + r.tone">{{ r.mark }} ›</span>
                </button>
              } @else {
                <div class="leg"><span>{{ r.name }} · {{ km(r.lengthM) }}</span><span class="mark">{{ r.mark }}</span></div>
              }
            }
          </div>
          @if (!ws.store.result() && ws.store.canEdit()) {
            <button type="button" class="rf-btn rf-btn--primary" (click)="ws.store.calculate()" [disabled]="busy()">{{ busy() ? 'Calculating…' : 'Calculate' }}</button>
          }
          @if (ws.batteryFailure()) {
            <div class="note bad">Battery check did not finish; the battery verdict is missing, not assumed fine.</div>
          }
          <div class="foot">{{ ws.store.canEdit() ? 'Editing the route works best on a laptop.' : 'View-only link. Ask the mission owner to change the route.' }}</div>
        }
      </section>
    }
  `,
  styles: `
    :host { position: relative; display: block; height: 100%; overflow: hidden; background: var(--rf-bg); }
    .map { position: absolute; left: 0; right: 0; top: 0; height: calc(56% - 28px); }
    .stage { position: absolute; inset: 0; }
    .chips { position: absolute; top: 12px; left: 12px; right: 12px; display: flex; justify-content: space-between; align-items: center; z-index: 5; pointer-events: none; }
    .chip { padding: 6px 10px; border-radius: 10px; background: rgba(16, 22, 29, 0.94); font-weight: 600; }
    .chip.role { padding: 4px 8px; border-radius: 6px; color: var(--rf-text-2); font-size: 12px; font-weight: 400; }
    .sheet { position: absolute; left: 0; right: 0; bottom: 0; top: calc(56% - 30px); z-index: 6; padding: 10px 16px calc(20px + env(safe-area-inset-bottom)); border-radius: 16px 16px 0 0; background: var(--rf-surface-2); border-top: 1px solid var(--rf-border); display: flex; flex-direction: column; gap: 12px; overflow-y: auto; }
    .handle { width: 40px; height: 4px; border-radius: 2px; background: var(--rf-border-strong); align-self: center; flex: none; }
    h2 { margin: 0; font-size: 20px; }
    .pills { display: flex; flex-wrap: wrap; gap: 6px; }
    .legs { display: flex; flex-direction: column; gap: 6px; }
    .leg { height: 44px; padding: 0 12px; border: 0; border-radius: 10px; background: var(--rf-surface-3); color: var(--rf-text); display: flex; justify-content: space-between; align-items: center; font-size: 14px; flex: none; }
    .leg.bad { border: 1px solid var(--rf-blocked-border); background: var(--rf-blocked-bg); }
    .mark { color: var(--rf-ok-text); font-weight: 600; }
    .mark.tone-blocked { color: var(--rf-blocked-text); }
    .mark.tone-warn { color: var(--rf-warn-text); }
    .foot { margin-top: auto; font-size: 12px; color: var(--rf-text-2); text-align: center; }
    .note.bad { padding: 10px; border-radius: 10px; background: var(--rf-blocked-bg); color: var(--rf-blocked-text); font-size: 13px; }
    .why { position: absolute; inset: 0; padding: 16px; display: flex; flex-direction: column; gap: 12px; overflow-y: auto; }
    .why-head { display: flex; align-items: center; gap: 8px; font-size: 18px; }
    .back { width: 44px; height: 44px; border: 0; border-radius: 10px; background: var(--rf-surface-3); color: var(--rf-text); font-size: 18px; flex: none; }
    .reason { padding: 12px; border-radius: 12px; background: var(--rf-blocked-bg); border: 1px solid var(--rf-blocked-border); color: var(--rf-text-3); line-height: 1.45; }
  `,
})
export class PhoneLayoutComponent {
  protected readonly ws = inject(FpvWorkspace);
  protected readonly padding = { top: 70, bottom: 70, left: 70, right: 90 };
  protected readonly busy = computed(() => ['saving', 'running'].includes(this.ws.store.calcState()));

  protected km(m: number): string {
    return RfFormat.format(m, 'km1');
  }

  protected readonly whyTitle = computed(() => {
    const link = this.ws.whyLink();
    const n = this.ws.why()?.leg;
    return link?.reachable === false ? `Why leg ${n} fails` : `Leg ${n}`;
  });

  protected readonly verdictText = computed(() => {
    const v = this.ws.verdict();
    const w = this.ws.why();
    if (!v || !w || v.legIndex !== w.leg) return null;
    const otherOk = (['control', 'video'] as const).filter((k) => k !== v.link).some((k) => this.ws.whyLeg()?.worst?.[k]?.reachable);
    const other = v.link === 'video' ? 'Control' : 'Video';
    const otherText = otherOk ? ` ${other} still works.` : '';
    return `${v.text}${otherText}`;
  });

  protected example(): void {
    this.ws.store.newMission(exampleMissionFor(this.ws.data));
  }
}
