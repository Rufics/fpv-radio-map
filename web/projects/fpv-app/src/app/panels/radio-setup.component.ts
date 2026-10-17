import { Component, OnInit, computed, inject, signal } from '@angular/core';
import { LinkKind, LinkSetup, RadioProfile, RfFormat, SPEED_OF_LIGHT } from '@rfmap/ui-kit';
import { FpvWorkspace } from '../core/fpv-workspace.service';

/** Sensitivity in dBm from a library profile: datasheet value, or S = 10log10(kT0B)+30+NF+SNR. */
export function sensitivityDbm(p: RadioProfile | undefined): number | null {
  if (!p) return null;
  const s = p.sensitivity;
  if (s.method === 'datasheet') return s.dBm;
  const k = 1.380649e-23;
  return 10 * Math.log10(k * 290 * s.bandwidthHz) + 30 + s.noiseFigureDb + (p.extraNoiseDb ?? 0) + s.requiredSnrDb;
}

/** Radio links (RadioSetup.dc.html): pick radio and antennas from the library; derived values read-only. */
@Component({
  selector: 'fpv-radio-setup',
  template: `
    <div class="backdrop" (click)="close()"></div>
    <section class="dialog" role="dialog" aria-modal="true" aria-labelledby="radio-title" (keydown.escape)="close()">
      <div class="head">
        <h2 id="radio-title">Radio links</h2>
        <button type="button" class="rf-icon-btn" aria-label="Close" (click)="close()">✕</button>
      </div>
      <nav class="tabs" role="tablist" aria-label="Link">
        @for (k of kinds; track k) {
          <button type="button" role="tab" [attr.aria-selected]="kind() === k" (click)="switchTo(k)">{{ k === 'control' ? 'Control' : 'Video' }}</button>
        }
      </nav>
      <div class="body">
        <div class="form">
          <label class="rf-field wide">Radio profile (library)
            <select class="rf-select" [value]="draft().radio" (change)="patch({ radio: $any($event.target).value })">
              @for (r of radios(); track r.id) {
                <option [value]="r.id">{{ r.name }} · rev {{ r.version }}{{ r.verified ? ' · verified' : ' · unverified' }}</option>
              }
            </select>
          </label>
          <label class="rf-field">Frequency, MHz<input class="rf-input rf-mono" [value]="freqText()" readonly /></label>
          <label class="rf-field">TX power, dBm
            <select class="rf-select rf-mono" [value]="draft().txPowerDbm" (change)="patch({ txPowerDbm: +$any($event.target).value })">
              @for (p of powers(); track p) {<option [value]="p">{{ fmt(p, 'dbm') }} · {{ watts(p) }}</option>}
            </select>
          </label>
          <label class="rf-field">Ground antenna (library)
            <select class="rf-select" [value]="draft().groundAntenna" (change)="patch({ groundAntenna: $any($event.target).value })">
              @for (a of ws.data.antennas(); track a.id) {<option [value]="a.id">{{ a.name }}{{ a.verified ? '' : ' · unverified' }}</option>}
            </select>
          </label>
          <label class="rf-field">Drone antenna (library)
            <select class="rf-select" [value]="draft().airAntenna" (change)="patch({ airAntenna: $any($event.target).value })">
              @for (a of ws.data.antennas(); track a.id) {<option [value]="a.id">{{ a.name }}{{ a.verified ? '' : ' · unverified' }}</option>}
            </select>
          </label>
          <label class="rf-field">Fade margin, dB
            <input class="rf-input rf-mono" type="number" min="0" step="0.5" [value]="draft().requiredMarginDb" (change)="patch({ requiredMarginDb: Math.max(0, +$any($event.target).value) })" />
          </label>
          <div class="rf-caption wide">Values come from the reference library and are never hard-coded. Out-of-band antennas are flagged below.</div>
          <ul class="prov wide" aria-label="Where the values come from">
            @for (p of provenance(); track p.role) {
              <li [class.unverified]="!p.verified">
                <span class="mark" aria-hidden="true">{{ p.verified ? '✓' : '!' }}</span>
                <span><b>{{ p.role }}</b> {{ p.name }} · {{ p.verified ? 'verified' : 'unverified' }}{{ p.source ? ' · ' + p.source : ' · no source' }}</span>
              </li>
            }
          </ul>
        </div>
        <dl class="derived rf-breakdown">
          <dt class="eyebrow">Derived, read-only</dt><dd></dd>
          <dt>EIRP</dt><dd [title]="full(d().eirp, 'dbm')">{{ fmt(d().eirp, 'dbm') }}</dd>
          <dt>Sensitivity (profile)</dt><dd>{{ fmt(d().sens, 'dbm') }}</dd>
          <dt>Required at receiver</dt><dd>{{ fmt(d().required, 'dbm') }}</dd>
          <dt>Max path loss</dt><dd>{{ fmt(d().maxLoss, 'db') }}</dd>
          <dt>Range in clear line of sight</dt><dd class="is-good" [title]="full(d().losRange, 'km3')">{{ fmt(d().losRange, 'km3') }}</dd>
          <dt>Wavelength</dt><dd>{{ d().lambda }}</dd>
        </dl>
      </div>
      @if (d().bandWarning; as w) {<div class="rf-field-error" role="alert">{{ w }}</div>}
      <div class="rf-card rf-card--ok note">Clear line of sight is the ceiling. Terrain, the Fresnel zone and Earth curvature cut it down on the map.</div>
      <div class="actions">
        <button type="button" class="rf-btn rf-btn--secondary" (click)="close()">Cancel</button>
        <span class="grow"></span>
        <button type="button" class="rf-btn rf-btn--primary" (click)="apply()">Apply to mission</button>
      </div>
    </section>
  `,
  styles: `
    .prov { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 4px; font-size: 12px; color: var(--rf-text-2); }
    .prov li { display: flex; gap: 6px; }
    .prov .mark { width: 12px; color: var(--rf-ok-text); flex: none; }
    .prov li.unverified { color: var(--rf-warn-text); }
    .prov li.unverified .mark { color: var(--rf-warn-text); }
    :host { position: absolute; inset: 0; z-index: 20; display: flex; align-items: center; justify-content: center; }
    .backdrop { position: absolute; inset: 0; background: rgba(11, 15, 20, 0.6); }
    .dialog { position: relative; width: 820px; max-width: calc(100% - 32px); max-height: calc(100% - 32px); overflow-y: auto; padding: 22px 24px; border-radius: 16px; background: var(--rf-surface-2); border: 1px solid var(--rf-border); display: flex; flex-direction: column; gap: 14px; }
    .head { display: flex; justify-content: space-between; align-items: center; }
    h2 { margin: 0; font-size: 20px; }
    .tabs { display: flex; gap: 4px; border-bottom: 1px solid var(--rf-border); }
    .tabs button { height: 44px; padding: 0 16px; border: 0; border-bottom: 3px solid transparent; background: transparent; color: var(--rf-text-2); }
    .tabs button[aria-selected='true'] { border-bottom-color: var(--rf-ok); color: var(--rf-text); font-weight: 600; }
    .body { display: flex; gap: 20px; flex-wrap: wrap; }
    .form { flex: 1 1 400px; display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 12px; }
    .wide { grid-column: 1 / 3; }
    .derived { flex: 1 1 260px; align-self: start; padding: 14px; border-radius: 12px; background: var(--rf-surface-1); border: 1px solid var(--rf-border); }
    .derived dt { color: var(--rf-text-3); font-family: var(--rf-font-sans); }
    .eyebrow { font-size: 12px; text-transform: uppercase; letter-spacing: 0.08em; color: var(--rf-admin) !important; font-weight: 600; }
    .note { font-size: 13px; color: var(--rf-text-3); }
    .actions { display: flex; gap: 10px; }
    .grow { flex: 1; }
  `,
})
export class RadioSetupComponent implements OnInit {
  protected readonly ws = inject(FpvWorkspace);
  protected readonly Math = Math;
  protected readonly kinds: LinkKind[] = ['control', 'video'];
  protected readonly kind = signal<LinkKind>('control');
  protected readonly drafts = signal<Partial<Record<LinkKind, LinkSetup>>>({});

  ngOnInit(): void {
    const m = this.ws.mission();
    this.kind.set(this.ws.radioSetup() ?? 'control');
    this.drafts.set(structuredClone(m?.links ?? {}));
  }

  protected readonly draft = computed<LinkSetup>(() => {
    const k = this.kind();
    const fallback = this.radios()[0];
    return this.drafts()[k] ?? { radio: fallback?.id ?? '', txPowerDbm: fallback?.txPowerDbm[0] ?? 20, groundAntenna: this.ws.data.antennas()[0]?.id ?? '', airAntenna: this.ws.data.antennas()[0]?.id ?? '', requiredMarginDb: 10 };
  });

  protected readonly radios = computed(() => this.ws.data.radios().filter((r) => (this.kind() === 'control' ? r.kind === 'control' : r.kind !== 'control')));
  protected readonly profile = computed(() => this.ws.data.radio(this.draft().radio));
  protected readonly powers = computed(() => this.profile()?.txPowerDbm ?? [this.draft().txPowerDbm]);
  protected readonly freqText = computed(() => ((this.profile()?.frequencyHz ?? 0) / 1e6).toFixed(3));

  /** Verified flag and source of each library entry in this link (feature 16). */
  protected readonly provenance = computed(() => {
    const dr = this.draft();
    const src = (x?: { kind?: string; reference?: string }) => (x?.reference ? `${x.kind}: ${x.reference}` : '');
    const radio = this.ws.data.radios().find((r) => r.id === dr.radio);
    const ant = (id: string) => this.ws.data.antennas().find((a) => a.id === id);
    const g = ant(dr.groundAntenna);
    const a = ant(dr.airAntenna);
    return [
      { role: 'Radio', name: radio?.name ?? dr.radio, verified: !!radio?.verified, source: src(radio?.sensitivity?.source) },
      { role: 'Ground antenna', name: g?.name ?? dr.groundAntenna, verified: !!g?.verified, source: src(g?.source) },
      { role: 'Drone antenna', name: a?.name ?? dr.airAntenna, verified: !!a?.verified, source: src(a?.source) },
    ];
  });

  protected readonly d = computed(() => {
    const s = this.draft();
    const p = this.profile();
    const g = this.ws.data.antenna(s.groundAntenna);
    const a = this.ws.data.antenna(s.airAntenna);
    const gains = (g?.gainDbi ?? 0) - (g?.feederLossDb ?? 0) + (a?.gainDbi ?? 0) - (a?.feederLossDb ?? 0);
    const eirp = s.txPowerDbm + (g?.gainDbi ?? 0) - (g?.feederLossDb ?? 0);
    const sens = sensitivityDbm(p);
    const required = sens !== null ? sens + s.requiredMarginDb : null;
    const maxLoss = required !== null ? s.txPowerDbm + gains - required : null;
    const f = p?.frequencyHz ?? 0;
    const losRange = maxLoss !== null && f ? 10 ** ((maxLoss - 20 * Math.log10(f / 1e6) - 32.44778) / 20) * 1000 : null;
    const lambda = f ? `${(SPEED_OF_LIGHT / f).toFixed(4)} m` : '—';
    const out = [g, a].filter((x) => x && f && (f < x.bandHz[0] || f > x.bandHz[1])).map((x) => x!.name);
    return { eirp, sens, required, maxLoss, losRange, lambda, bandWarning: out.length ? `Outside the antenna band: ${out.join(', ')}.` : null };
  });

  protected fmt(v: number | null | undefined, k: 'dbm' | 'db' | 'km3'): string {
    return RfFormat.format(v, k);
  }
  protected full(v: number | null | undefined, k: 'dbm' | 'km3'): string {
    return RfFormat.full(v, k);
  }
  protected watts(dbm: number): string {
    const w = 10 ** (dbm / 10) / 1000;
    return w >= 1 ? `${w.toFixed(3)} W` : `${Math.round(w * 1000)} mW`;
  }

  protected patch(p: Partial<LinkSetup>): void {
    const k = this.kind();
    const next = { ...this.draft(), ...p };
    if (p.radio) {
      const prof = this.ws.data.radio(p.radio);
      if (prof && !prof.txPowerDbm.includes(next.txPowerDbm)) next.txPowerDbm = Math.max(...prof.txPowerDbm);
    }
    this.drafts.update((d) => ({ ...d, [k]: next }));
  }

  protected switchTo(k: LinkKind): void {
    this.kind.set(k);
  }

  protected apply(): void {
    const drafts = this.drafts();
    this.ws.store.update((m) => ({ ...m, links: { ...m.links, ...drafts } }));
    this.close();
  }

  protected close(): void {
    this.ws.radioSetup.set(null);
  }
}
