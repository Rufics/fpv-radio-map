import { Component, computed, inject, signal } from '@angular/core';
import { LinkKind, RfFormat, RfNumComponent, SegmentedComponent, linksEvaluated } from '@rfmap/ui-kit';
import { CompareService } from '../core/fpv-core';
import { FpvDataService } from '../core/fpv-data.service';
import { FpvWorkspace, LINK_LETTER } from '../core/fpv-workspace.service';

type Change = 'video-radio' | 'control-radio' | 'video-power' | 'video-antenna' | 'drone' | 'custom';

/** Feature 13: compare scenario A (this mission) with B (a JSON merge patch on it). */
@Component({
  selector: 'fpv-compare-panel',
  imports: [RfNumComponent, SegmentedComponent],
  template: `
    <section class="card" aria-labelledby="cmp-title">
      <div class="head">
        <b id="cmp-title">Compare a scenario</b>
        <span class="rf-badge rf-badge--beta">fpv.core · beta</span>
      </div>
      @if (!cmp.available()) {
        <div class="muted">Module fpv.core is not installed on this server, so scenarios cannot be compared.</div>
      } @else if (!cmp.options()) {
        @if (canEdit()) {
          <label class="field"><span>Name of B</span><input class="rf-input" [value]="name()" (input)="name.set($any($event.target).value)" maxlength="60" /></label>
          <label class="field"><span>What changes in B</span>
            <select class="rf-input" [value]="change()" (change)="setChange($any($event.target).value)">
              <option value="video-radio">Video radio</option>
              <option value="control-radio">Control radio</option>
              <option value="video-power">Video transmit power</option>
              <option value="video-antenna">Video ground antenna</option>
              <option value="drone">Airframe</option>
              <option value="custom">Custom JSON merge patch</option>
            </select>
          </label>
          @switch (change()) {
            @case ('video-radio') {
              <label class="field"><span>Video radio in B</span><select class="rf-input" (change)="value.set($any($event.target).value)">
                <option value="">Choose…</option>
                @for (r of videoRadios(); track r.id) {<option [value]="r.id">{{ r.name }}{{ r.verified ? '' : ' · unverified' }}</option>}
              </select></label>
            }
            @case ('control-radio') {
              <label class="field"><span>Control radio in B</span><select class="rf-input" (change)="value.set($any($event.target).value)">
                <option value="">Choose…</option>
                @for (r of controlRadios(); track r.id) {<option [value]="r.id">{{ r.name }}{{ r.verified ? '' : ' · unverified' }}</option>}
              </select></label>
            }
            @case ('video-power') {
              <label class="field"><span>Video power in B, dBm</span><input class="rf-input rf-num" type="number" step="1" min="0" max="40" (input)="value.set($any($event.target).value)" /></label>
            }
            @case ('video-antenna') {
              <label class="field"><span>Ground antenna in B</span><select class="rf-input" (change)="value.set($any($event.target).value)">
                <option value="">Choose…</option>
                @for (a of data.antennas(); track a.id) {<option [value]="a.id">{{ a.name }}{{ a.verified ? '' : ' · unverified' }}</option>}
              </select></label>
            }
            @case ('drone') {
              <label class="field"><span>Airframe in B</span><select class="rf-input" (change)="value.set($any($event.target).value)">
                <option value="">Choose…</option>
                @for (d of data.drones(); track d.id) {<option [value]="d.id">{{ d.name }}{{ d.verified ? '' : ' · unverified' }}</option>}
              </select></label>
            }
            @case ('custom') {
              <label class="field"><span>JSON merge patch on the mission</span>
                <textarea class="rf-input rf-mono json" spellcheck="false" [value]="value()" (input)="value.set($any($event.target).value)" placeholder='{"links":{"video":{"txPowerDbm":30}}}'></textarea>
              </label>
            }
          }
          @if (patchError(); as e) {<div class="rf-field-error" role="alert">{{ e }}</div>}
          <button type="button" class="rf-btn rf-btn--secondary" [disabled]="!patch() || busy()" (click)="run()">Compare A and B</button>
        } @else {
          <div class="muted">No scenario B in this mission.</div>
        }
      } @else {
        <div class="muted small">B = “{{ cmp.options()?.name || 'B' }}” · patch <code>{{ patchText() }}</code></div>
        @if (res(); as r) {
          @if (!r.aligned) {
            <div class="note">B moves the route, so only the worst margins are compared; there is no point-by-point difference.</div>
          }
          <div class="table" role="table" aria-label="Scenario A and B">
            <div class="tr th" role="row"><span role="columnheader">Worst margin, dB</span><span role="columnheader">A</span><span role="columnheader">B</span><span role="columnheader">B − A</span></div>
            @for (l of rows(); track l.kind) {
              <div class="tr" role="row">
                <span role="rowheader">{{ l.letter }} · {{ l.kind }}</span>
                <span role="cell" [class.bad]="l.a < 0"><rf-num [value]="l.a" kind="db+" [unit]="false" /></span>
                <span role="cell" [class.bad]="l.b < 0"><rf-num [value]="l.b" kind="db+" [unit]="false" /></span>
                <span role="cell" [class.good]="l.d > 0" [class.bad]="l.d < 0">{{ l.mark }} <rf-num [value]="l.d" kind="db+" [unit]="false" /></span>
              </div>
              @if (l.better !== undefined) {
                <div class="counts small" role="row"><span role="cell">{{ l.letter }}: ▲ better at {{ l.better }} points · ▼ worse at {{ l.worse }} of {{ l.total }} (every {{ stepText() }})</span></div>
              }
            }
          </div>
          @if (r.aligned) {
            <div class="row">
              <span class="small muted">Route colour shows</span>
              <rf-segmented [options]="linkOptions()" [value]="cmp.link()" (valueChange)="cmp.link.set($event)" label="Link shown on the route" />
            </div>
          }
          <div class="row">
            <button type="button" class="rf-btn rf-btn--dense rf-btn--secondary" [disabled]="cmp.diffState() === 'running' || !hasCoverage()" (click)="cmp.coverageDiff(cmp.link())">
              {{ cmp.diffState() === 'running' ? 'Calculating B coverage…' : 'Coverage difference map' }}
            </button>
            @if (cmp.delta()) {<button type="button" class="rf-btn rf-btn--dense rf-btn--secondary" (click)="cmp.clearDiff()">Hide</button>}
          </div>
          @if (!hasCoverage()) {<div class="small muted">The difference map needs this mission's coverage; it is calculated with Calculate.</div>}
          @if (cmp.diffError(); as e) {<div class="rf-field-error" role="alert">{{ e }}</div>}
          @if (cmp.delta(); as d) {
            <div class="small muted">Map: ▲ teal where B is better, ▼ coral where B is worse ({{ cmp.link() }} link, B − A, 1 dB and more).</div>
            @if (d.notComparableCells > 0) {
              <div class="note">{{ d.notComparableCells }} cells are not comparable (hatched): B's grid does not cover them with exactly the same cells. Nothing is resampled.</div>
            }
          }
        } @else {
          <div class="muted small">{{ ws.store.calcState() === 'running' || ws.store.calcState() === 'saving' ? 'Calculating A and B…' : 'Calculate to compare.' }}</div>
        }
        @if (canEdit()) {
          <button type="button" class="rf-btn rf-btn--dense rf-btn--secondary" (click)="cmp.remove()">Remove scenario B</button>
        }
      }
    </section>
  `,
  styles: `
    .card { display: flex; flex-direction: column; gap: 10px; padding: 14px; border-radius: 12px; border: 1px solid var(--rf-border); background: var(--rf-surface-1); }
    .head { display: flex; justify-content: space-between; align-items: center; gap: 8px; }
    .field { display: flex; flex-direction: column; gap: 4px; font-size: 12px; color: var(--rf-text-2); }
    .json { height: 72px; padding: 8px; resize: vertical; font-size: 12px; }
    .muted { color: var(--rf-text-2); }
    .small { font-size: 12px; }
    code { font-size: 11px; word-break: break-all; }
    .note { padding: 8px 10px; border-radius: 8px; background: var(--rf-warn-bg); color: var(--rf-warn-text); font-size: 12px; }
    .table { border: 1px solid var(--rf-border); border-radius: 8px; overflow: hidden; font-size: 13px; }
    .tr { display: grid; grid-template-columns: minmax(0, 1.4fr) 1fr 1fr 1.1fr; border-top: 1px solid var(--rf-divider); }
    .tr > span { padding: 6px 8px; white-space: nowrap; }
    .tr > span:not(:first-child) { text-align: right; font-family: var(--rf-font-mono); }
    .tr.th { background: var(--rf-surface-3); color: var(--rf-text-2); font-size: 12px; font-weight: 600; border-top: 0; }
    .counts { padding: 0 8px 6px; color: var(--rf-text-2); }
    .good { color: var(--rf-ok-text); }
    .bad { color: var(--rf-blocked-text); }
    .row { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
  `,
})
export class ComparePanelComponent {
  protected readonly cmp = inject(CompareService);
  protected readonly data = inject(FpvDataService);
  protected readonly ws = inject(FpvWorkspace);
  protected readonly name = signal('B');
  protected readonly change = signal<Change>('video-radio');
  protected readonly value = signal('');
  protected readonly canEdit = computed(() => this.ws.store.canEdit());
  protected readonly busy = computed(() => ['saving', 'running'].includes(this.ws.store.calcState()));
  protected readonly res = computed(() => this.cmp.result());
  protected readonly hasCoverage = computed(() => !!this.ws.store.coverage()[this.cmp.link()]);
  protected readonly videoRadios = computed(() => this.data.radios().filter((r) => r.kind !== 'control'));
  protected readonly controlRadios = computed(() => this.data.radios().filter((r) => r.kind === 'control'));
  protected readonly linkOptions = computed(() =>
    linksEvaluated(this.ws.mission()?.evaluate ?? 'both').map((k) => ({ value: k, label: LINK_LETTER[k] })),
  );

  protected setChange(c: Change): void {
    this.change.set(c);
    this.value.set('');
  }

  private readonly parsed = computed<{ patch: Record<string, unknown> | null; error: string | null }>(() => {
    const v = this.value().trim();
    if (!v) return { patch: null, error: null };
    switch (this.change()) {
      case 'video-radio':
        return { patch: { links: { video: { radio: v } } }, error: null };
      case 'control-radio':
        return { patch: { links: { control: { radio: v } } }, error: null };
      case 'video-power': {
        const n = Number(v);
        return Number.isFinite(n) ? { patch: { links: { video: { txPowerDbm: n } } }, error: null } : { patch: null, error: 'Enter a number.' };
      }
      case 'video-antenna':
        return { patch: { links: { video: { groundAntenna: v } } }, error: null };
      case 'drone':
        return { patch: { drone: v }, error: null };
      case 'custom':
        try {
          const p = JSON.parse(v);
          if (!p || typeof p !== 'object' || Array.isArray(p)) return { patch: null, error: 'The patch must be a JSON object.' };
          return { patch: p, error: null };
        } catch (e) {
          return { patch: null, error: `Not valid JSON: ${(e as Error).message}` };
        }
    }
  });
  protected readonly patch = computed(() => this.parsed().patch);
  protected readonly patchError = computed(() => this.parsed().error);
  protected readonly patchText = computed(() => JSON.stringify(this.cmp.options()?.patch ?? {}));
  protected readonly stepText = computed(() => RfFormat.format(this.res()?.stepM ?? 100, 'm'));

  protected readonly rows = computed(() => {
    const links = this.res()?.links ?? {};
    return (Object.keys(links) as LinkKind[]).map((kind) => {
      const l = links[kind]!;
      const a = l.a?.worstMarginDb ?? Number.NaN;
      const b = l.b?.worstMarginDb ?? Number.NaN;
      const d = l.deltaWorstDb ?? b - a;
      return {
        kind,
        letter: LINK_LETTER[kind],
        a,
        b,
        d,
        mark: this.deltaMark(d),
        better: l.pointsBetter,
        worse: l.pointsWorse,
        total: l.points?.length,
      };
    });
  });

  protected async run(): Promise<void> {
    const p = this.patch();
    if (p) await this.cmp.compare(this.name().trim(), p);
  }

  /** '=' for no change, '▲' when B is better, '▼' otherwise (also when the difference is unknown). */
  private deltaMark(d: number): string {
    if (Math.abs(d) < 0.005) return '=';
    return d > 0 ? '▲' : '▼';
  }
}
