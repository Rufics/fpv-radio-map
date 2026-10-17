import { Component, computed, inject, input, signal } from '@angular/core';
import {
  BudgetChartComponent,
  LinkKind,
  ProfileChartComponent,
  RfFormat,
  RfNumComponent,
  SlotHostComponent,
  groundToAir,
} from '@rfmap/ui-kit';
import { FpvWorkspace, LINK_LETTER } from '../core/fpv-workspace.service';

/** "Show why": terrain profile with the Fresnel zone, the full budget, uncertainty notes, fix preview. */
@Component({
  selector: 'fpv-why-drawer',
  imports: [ProfileChartComponent, BudgetChartComponent, RfNumComponent, SlotHostComponent],
  template: `
    @if (vm(); as v) {
      <div class="left">
        <div class="head">
          <h2 id="why-title">{{ compact() ? v.subtitle : v.title }}</h2>
          <div class="tools">
            <div class="rf-segmented small" role="group" aria-label="Link">
              @for (k of v.links; track k.kind) {
                <button type="button" [attr.aria-pressed]="k.kind === v.kind" (click)="ws.openWhy(v.leg, k.kind)">{{ k.label }}</button>
              }
            </div>
            <div class="rf-segmented small" role="group" aria-label="Chart">
              <button type="button" [attr.aria-pressed]="tab() === 'profile'" (click)="tab.set('profile')">Profile</button>
              <button type="button" [attr.aria-pressed]="tab() === 'budget'" (click)="tab.set('budget')">Budget</button>
            </div>
            @if (!compact()) {
              <button type="button" class="rf-icon-btn" aria-label="Close why drawer" (click)="ws.why.set(null)">✕</button>
            }
          </div>
        </div>
        <div class="chart" [class.budget]="tab() === 'budget'">
          @if (tab() === 'profile') {
            <rf-profile-chart
              [profile]="v.view.profile"
              [fromAglM]="v.fromAgl"
              [toAglM]="v.toAgl"
              [fromAntennaMslM]="v.view.ground?.antennaMslM"
              [toAntennaMslM]="v.view.air?.antennaMslM"
              [frequencyHz]="v.link.frequencyHz ?? 915e6"
              [kFactor]="v.k"
              [diffraction]="v.view.diffraction"
              [lossDb]="v.link.diffraction?.lossDb"
              obstacleName="Ridge"
            />
          } @else {
            <rf-budget-chart [terms]="v.link.terms" [receivedDbm]="v.link.receivedDbm" [requiredDbm]="v.link.requiredDbm" />
          }
        </div>
      </div>
      <div class="right">
        <dl class="rf-breakdown">
          <dt [title]="'Ray length ' + (v.link.rayLengthM ?? '')">Distance</dt><dd><rf-num [value]="v.link.distanceM ?? v.link.rayLengthM" kind="km3" /></dd>
          @for (t of v.terms; track t.id) {
            <dt [title]="t.source">{{ t.label }}</dt><dd><rf-num [value]="t.value" [kind]="t.kind" /></dd>
          }
          @if (v.link.diffraction?.firstFresnelRadiusM !== undefined) {
            <dt>Fresnel r1 at ridge</dt><dd><rf-num [value]="v.link.diffraction.firstFresnelRadiusM" kind="m2" /></dd>
          }
          @if (v.link.diffraction?.nu !== undefined && v.link.diffraction.lineOfSight === false) {
            <dt>Diffraction ν</dt><dd><rf-num [value]="v.link.diffraction.nu" kind="n2" /></dd>
          }
          <dt>Received</dt><dd><rf-num [value]="v.link.receivedDbm" kind="dbm" /></dd>
          <dt>Required</dt><dd><rf-num [value]="v.link.requiredDbm" kind="dbm" /></dd>
          <dt>Margin</dt><dd [class.is-bad]="(v.link.marginDb ?? 0) < 0" [class.is-good]="(v.link.marginDb ?? 0) >= 0">
            <rf-num [value]="v.link.marginDb" kind="db+" /> {{ (v.link.marginDb ?? 0) < 0 ? '✕' : '✓' }}
          </dd>
        </dl>
        @if (v.fix && ws.store.canEdit() && !ws.store.preview()) {
          <button type="button" class="rf-btn rf-btn--primary" (click)="ws.store.previewFix(v.fix)">Preview fix: {{ v.fixTitle }}</button>
        } @else if (v.fix) {
          <div class="fixnote"><b>Fix:</b> {{ v.fix.title }}.</div>
        }
        @for (n of v.link.uncertainty ?? []; track $index) {
          <div class="note"><b>{{ n.what }}</b> {{ n.detail }}</div>
        }
        @if (v.link.model) {
          <div class="note"><b class="plain">Model and data used</b> {{ v.link.model }}{{ v.link.profile?.source ? ' · ' + v.link.profile.source : '' }}</div>
        }
        <rf-slot-host name="bottom.drawer" [data]="v" />
      </div>
    }
  `,
  styles: `
    :host { display: grid; grid-template-columns: minmax(0, 2fr) minmax(0, 1fr); gap: 16px; padding: 16px; background: var(--rf-surface-1); border-top: 1px solid var(--rf-border); }
    :host(.compact) { grid-template-columns: minmax(0, 1fr); padding: 0; border: 0; background: transparent; }
    .left { display: flex; flex-direction: column; gap: 8px; min-width: 0; }
    .head { display: flex; justify-content: space-between; align-items: center; gap: 10px; flex-wrap: wrap; }
    h2 { margin: 0; font-size: 16px; }
    .tools { display: flex; gap: 8px; align-items: center; }
    .small > button { height: 30px; padding: 0 10px; font-size: 12px; flex: none; }
    .chart { position: relative; height: 230px; border-radius: 8px; overflow: hidden; }
    .chart.budget { height: auto; min-height: 230px; padding: 8px; background: var(--rf-surface-1); border: 1px solid var(--rf-border); }
    :host(.compact) .chart { height: 170px; border-radius: 12px; }
    .right { display: flex; flex-direction: column; gap: 10px; max-height: 300px; overflow-y: auto; padding-right: 4px; }
    .right > * { flex: none; }
    :host(.compact) .right { max-height: none; padding: 12px; border-radius: 12px; background: var(--rf-surface-2); }
    .note { font-size: 12px; line-height: 1.45; color: var(--rf-text-3); padding: 8px 10px; border-radius: 8px; background: var(--rf-surface-2); border: 1px solid var(--rf-border); }
    .note b { color: var(--rf-admin); }
    .note b.plain { color: var(--rf-text); }
    .fixnote { padding: 12px; border-radius: 12px; background: var(--rf-ok-bg); line-height: 1.45; }
    .fixnote b { color: var(--rf-ok-text); }
  `,
  host: { role: 'region', 'aria-labelledby': 'why-title', '[class.compact]': 'compact()' },
})
export class WhyDrawerComponent {
  protected readonly ws = inject(FpvWorkspace);
  readonly compact = input(false);
  protected readonly tab = signal<'profile' | 'budget'>('profile');

  protected readonly vm = computed(() => {
    const w = this.ws.why();
    const leg = this.ws.whyLeg();
    const link = this.ws.whyLink();
    const m = this.ws.mission();
    if (!w || !leg || !link || !m) return null;
    const failing = link.reachable === false;
    const mhz = RfFormat.format(link.frequencyHz, 'mhz');
    const to = leg.to ?? 'B';
    // where along the leg the worst sample was taken
    const along = link.sample?.alongLegM;
    const atEnd = along === undefined || leg.lengthM === undefined || Math.abs(leg.lengthM - along) < 1;
    const where = atEnd ? `at ${to}` : along! < 1 ? `at ${leg.from ?? 'the start'}` : `${RfFormat.format(along, 'km1')} into the leg`;
    const subtitle = `A → drone ${where} · ${w.link} ${mhz}`;
    const title = `${failing ? 'Why leg' : 'Leg'} ${w.leg}${failing ? ' fails' : ' · reachable'} · ${subtitle}`;
    const links = (['control', 'video'] as LinkKind[])
      .filter((k) => leg.worst?.[k])
      .map((k) => ({ kind: k, label: `${LINK_LETTER[k]} ${leg.worst?.[k]?.reachable === false ? '✕' : '✓'}` }));
    const terms = (link.terms ?? [])
      .filter((t) => t.valueDb > 0)
      .map((t) => ({ id: t.id, label: t.id === 'fspl' ? 'Free-space loss' : t.id === 'diffraction' ? 'Ridge diffraction' : t.label, value: t.valueDb, source: t.source, kind: 'db' as const }));
    const gains = (link.terms ?? []).filter((t) => t.valueDb <= 0);
    const toPoint = m.waypoints[w.leg - 1] ?? m.pilot;
    const fix = (leg.reasons ?? []).find((r) => r.fix && (!r.link || r.link === w.link))?.fix;
    const view = groundToAir(link);
    return {
      leg: w.leg,
      view,
      kind: w.link,
      title,
      subtitle,
      links,
      link,
      terms,
      gains,
      // heights from LinkResult.ends; the mission only when an older result has no ends
      fromAgl: view.ground?.aglM ?? m.pilot.aglM,
      toAgl: view.air?.aglM ?? link.sample?.aglM ?? toPoint.aglM,
      k: link.earth?.effectiveEarthFactor ?? this.ws.store.result()?.weather?.effectiveEarthFactor,
      fix,
      fixTitle: fix?.title ? fix.title.replace(/^Fly /, '').replace(/^./, (c) => c.toLowerCase()) : '',
    };
  });
}
