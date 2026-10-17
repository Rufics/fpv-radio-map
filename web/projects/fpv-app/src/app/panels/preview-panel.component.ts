import { Component, computed, inject } from '@angular/core';
import { FpvWorkspace } from '../core/fpv-workspace.service';

/** A fix is never applied directly: before/after table + map preview → Apply / Cancel. */
@Component({
  selector: 'fpv-preview-panel',
  template: `
    @if (ws.store.preview(); as p) {
      <div class="rf-eyebrow">Preview · nothing changed yet</div>
      <h2>{{ p.fix.title }}</h2>
      <table class="table">
        <caption class="sr-only">Values now and after the change</caption>
        <thead><tr><th scope="col"><span class="sr-only">Value</span></th><th scope="col">Now</th><th scope="col">After</th></tr></thead>
        <tbody>
          @for (r of rows(); track $index) {
            <tr>
              <th scope="row">{{ r.label }}</th>
              <td class="now">{{ r.now }}</td>
              <td class="after" [class]="'after ' + (r.trend ?? 'neutral')">
                {{ r.after }}
                @if (r.trend === 'better') {<span class="sr-only">(better)</span>}
                @if (r.trend === 'worse') {<span class="sr-only">(worse)</span>}
              </td>
            </tr>
          }
        </tbody>
      </table>
      @if (p.fix.note) {
        <div class="note">{{ p.fix.note }}</div>
      }
      @if (!ws.store.canEdit()) {
        <div class="note">View-only link. Ask the mission owner to change the route.</div>
      }
      <div class="actions">
        <button type="button" class="rf-btn rf-btn--primary" (click)="apply()" [disabled]="!ws.store.canEdit()">Apply</button>
        <button type="button" class="rf-btn rf-btn--secondary" (click)="ws.store.cancelPreview()">Cancel</button>
      </div>
    }
  `,
  styles: `
    :host { display: flex; flex-direction: column; gap: 14px; }
    h2 { margin: 0; font-size: 20px; }
    .table { width: 100%; border-collapse: separate; border-spacing: 0; border: 1px solid var(--rf-border); border-radius: 12px; overflow: hidden; font-size: 13px; }
    thead th { background: var(--rf-surface-3); color: var(--rf-text-2); font-size: 12px; font-weight: 600; text-align: right; padding: 8px 10px; }
    tbody th { font-weight: 400; text-align: left; padding: 8px 10px; border-top: 1px solid var(--rf-divider); width: 44%; }
    td { padding: 8px 10px; text-align: right; font-family: var(--rf-font-mono); font-variant-numeric: tabular-nums; border-top: 1px solid var(--rf-divider); }
    td:first-of-type { white-space: nowrap; }
    .now { color: var(--rf-text-2); }
    .after.better { color: var(--rf-ok-text); }
    .after.worse { color: var(--rf-blocked-text); }
    .after.caution { color: var(--rf-warn-text); }
    .note { font-size: 13px; color: var(--rf-text-2); line-height: 1.45; }
    .actions { display: flex; gap: 8px; }
    .actions .rf-btn { flex: 1; }
  `,
})
export class PreviewPanelComponent {
  protected readonly ws = inject(FpvWorkspace);
  protected readonly rows = computed(() => (this.ws.store.preview()?.fix.preview ?? []).map((r) => ({ ...r, trend: r.trend as string | undefined })));

  protected async apply(): Promise<void> {
    this.ws.why.set(null);
    await this.ws.store.applyPreview();
  }
}
