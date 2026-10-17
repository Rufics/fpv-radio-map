import { Component, computed, inject, signal } from '@angular/core';
import { MapContext, RfFormat } from '@rfmap/ui-kit';
import { FpvWorkspace } from '../core/fpv-workspace.service';

/** States from States.dc.html: missing elevation data, module failed, stale forecast, beyond horizon, errors. */
@Component({
  selector: 'fpv-state-cards',
  template: `
    @if (ws.store.error(); as err) {
      <section class="rf-card rf-card--blocked card" role="alert">
        <div class="rf-eyebrow bad">Calculation problem</div>
        <div class="rf-body">{{ err }}</div>
        <button type="button" class="rf-btn rf-btn--secondary" (click)="ws.store.calculate()">Try again</button>
      </section>
    }
    @if (ws.missingLeg(); as leg) {
      <section class="rf-card rf-card--blocked card" role="alert">
        <div class="rf-eyebrow bad">Missing data</div>
        <h3>No elevation data on leg {{ leg.index }}</h3>
        <div class="rf-body">{{ leg.reasons?.[0]?.text ?? 'Part of this leg is outside the loaded region. That part is not calculated: we do not guess the terrain.' }}</div>
        <div class="box rf-mono">Leg {{ leg.index }} · {{ leg.from }} → {{ leg.to }} · {{ km(leg.lengthM) }} not calculated</div>
        <button type="button" class="rf-btn rf-btn--primary" (click)="askAdmin(leg.index ?? 0)">{{ copied() === 'area' ? 'Request copied' : 'Ask admin to add this area' }}</button>
        <button type="button" class="rf-btn rf-btn--secondary" (click)="showCalculated()">Show the calculated part</button>
      </section>
    }
    @for (f of failures(); track f.module) {
      <section class="rf-card rf-card--blocked card" role="alert">
        <div class="rf-eyebrow bad">Module failed</div>
        <h3>{{ f.title }} did not finish</h3>
        <div class="rf-body">Module <code>{{ f.module }}</code> {{ f.error }}. Radio results are shown; the {{ f.what }} verdict is missing, not assumed fine.</div>
        <button type="button" class="rf-btn rf-btn--primary" (click)="ws.store.calculate()">Try again</button>
        <button type="button" class="rf-btn rf-btn--secondary" (click)="copyReport(f.module + ': ' + f.error)">{{ copied() === f.module ? 'Error report copied' : 'Copy error report for admin' }}</button>
      </section>
    }
    @if (stale(); as s) {
      <section class="rf-card rf-card--warn card" role="status">
        <div class="rf-eyebrow warn">Old data</div>
        <h3>Forecast is {{ s.hours }} hours old</h3>
        <div class="rf-body">The server cannot reach the weather source. Wind and refraction use the last forecast, and every result that depends on it carries this note.</div>
        <div class="box">Forecast issued {{ s.issued }} · now {{ s.now }}</div>
        @if (ws.store.canEdit()) {
          <button type="button" class="rf-btn rf-btn--secondary" (click)="withoutWeather()">Calculate without weather</button>
        }
      </section>
    }
    @if (beyond()) {
      <section class="rf-card rf-card--plain card" role="status">
        <div class="rf-eyebrow">Beyond the forecast horizon</div>
        <div class="rf-body">No forecast exists for this time. Results use the standard atmosphere (ISA, k = 4/3) and say so; we do not invent weather.</div>
      </section>
    }
  `,
  styles: `
    :host { display: contents; }
    .card { display: flex; flex-direction: column; gap: 10px; padding: 14px; }
    h3 { margin: 0; font-size: 16px; }
    .bad { color: var(--rf-blocked-text); }
    .warn { color: var(--rf-warn-text); }
    .box { padding: 8px 10px; border-radius: 8px; background: var(--rf-surface-2); font-size: 12px; color: var(--rf-text-2); }
  `,
})
export class StateCardsComponent {
  protected readonly ws = inject(FpvWorkspace);
  private readonly ctx = inject(MapContext);
  protected readonly copied = signal<string | null>(null);

  protected km(m: number | undefined): string {
    return RfFormat.format(m, 'km1');
  }

  protected readonly failures = computed(() =>
    (this.ws.store.result()?.moduleFailures ?? []).map((f) => {
      const id = (f.module ?? '').split(' ')[0];
      const title = id === 'fpv.battery' ? 'Battery check' : (this.ws.data.moduleTitles().get(id) ?? id);
      return { module: f.module ?? 'module', error: f.error ?? 'failed', title, what: id === 'fpv.battery' ? 'battery' : id };
    }),
  );

  protected readonly stale = computed(() => {
    const w = this.ws.store.result()?.weather;
    if (!w?.stale || !w.runUtc) return null;
    const run = new Date(w.runUtc);
    const hm = (d: Date) => d.toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit' });
    return { hours: Math.round((Date.now() - run.getTime()) / 3600e3), issued: hm(run), now: hm(new Date()) };
  });

  protected readonly beyond = computed(() => this.ws.store.result()?.weather?.tier === 'none');

  protected async withoutWeather(): Promise<void> {
    this.ws.store.update((m) => {
      const modules = { ...(m.modules ?? {}) };
      delete modules['rf.weather'];
      return { ...m, modules };
    });
    await this.ws.store.calculate();
  }

  protected showCalculated(): void {
    const m = this.ws.mission();
    const missing = this.ws.missingLeg()?.index ?? 0;
    if (!m) return;
    const pts = [m.pilot, ...m.waypoints].slice(0, Math.max(2, missing));
    this.ctx.fitTo(pts, 80);
  }

  protected async askAdmin(leg: number): Promise<void> {
    const m = this.ws.mission();
    if (!m) return;
    const pts = [m.pilot, ...m.waypoints];
    const lats = pts.map((p) => p.lat);
    const lons = pts.map((p) => p.lon);
    const text = `Please add elevation data for mission "${m.name}", leg ${leg}: ${Math.min(...lats).toFixed(2)}–${Math.max(...lats).toFixed(2)} N, ${Math.min(...lons).toFixed(2)}–${Math.max(...lons).toFixed(2)} E`;
    await this.copy(text, 'area');
  }

  protected copyReport(text: string): Promise<void> {
    const r = this.ws.store.result();
    return this.copy(`rfmap module failure\n${text}\nmission revision ${r?.missionRevision ?? '?'} · input ${r?.inputHash ?? '?'}\n${new Date().toISOString()}`, text.split(':')[0]);
  }

  private async copy(text: string, key: string): Promise<void> {
    try {
      await navigator.clipboard.writeText(text);
    } catch {
      /* clipboard blocked: nothing else to do */
    }
    this.copied.set(key);
    setTimeout(() => this.copied.set(null), 2500);
  }
}
