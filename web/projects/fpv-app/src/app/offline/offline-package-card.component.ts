import { Component, computed, inject, input } from '@angular/core';
import { OfflineAreaRequest, OfflineRegion, SavedPackageKind } from '@rfmap/ui-kit';
import { OfflineLibrary } from './offline-library.service';
import { OfflineRegionFormat } from './offline-region-format';

/**
 * One package a client can take offline: its preview and facts, and whether it is on this
 * device: save it, follow the download, or update a saved copy older than the catalog's.
 */
@Component({
  selector: 'fpv-offline-package-card',
  template: `
    @let r = region();
    @if (r.preview) {
      <img class="preview" [src]="r.preview" [alt]="'Relief of ' + r.name" width="256" height="171" />
    } @else {
      <div class="preview none" aria-hidden="true">No preview</div>
    }
    <div class="body">
      <h2>{{ r.name }}</h2>
      <span class="muted small rf-num">{{ f.bbox(r) }}</span>
      <dl>
        <dt>Size</dt>
        <dd class="rf-num">{{ f.size(r.size) }}</dd>
        <dt>Layers</dt>
        <dd>{{ f.layers(r) }}</dd>
        <dt>Detail</dt>
        <dd>{{ f.zoom(r) }}</dd>
        <dt>Version</dt>
        <dd>
          <span class="rf-mono" [attr.title]="r.version">{{ f.version(r) }}</span> ·
          {{ f.builtOn(r) }}
        </dd>
      </dl>
      @if (progress(); as p) {
        <div class="saving" role="status">
          <div class="bar" role="progressbar" [attr.aria-valuenow]="percent()" aria-valuemin="0" aria-valuemax="100"
            [attr.aria-label]="'Saving ' + r.name">
            <div class="fill" [style.width.%]="percent()"></div>
          </div>
          <span class="small rf-num">Saving… {{ f.size(p.received) }} of {{ f.size(p.total) }}</span>
        </div>
        <div class="actions">
          <button type="button" class="rf-btn rf-btn--secondary" (click)="library.cancel(key())">Cancel</button>
        </div>
      } @else {
        @if (saved(); as s) {
          @if (outdated()) {
            <p class="note update" role="status">
              Update available: this device has version
              <span class="rf-mono">{{ f.version(s.region) }}</span> of {{ f.builtOn(s.region) }}.
            </p>
          } @else {
            <p class="note ok">On this device, saved {{ f.savedOn(s.savedAt) }}.</p>
          }
        }
        <div class="actions">
          @if (!saved() || outdated()) {
            <button type="button" class="rf-btn rf-btn--primary"
              [attr.aria-label]="(outdated() ? 'Update ' : 'Save on this device: ') + r.name + ', ' + f.size(r.size)"
              (click)="save()">{{ outdated() ? 'Update' : 'Save on this device' }}</button>
          }
          <a class="rf-btn rf-btn--secondary" [href]="r.package" download
            [attr.aria-label]="'Download ' + r.name + ', ' + f.size(r.size)">Download zip</a>
        </div>
      }
      @if (failure(); as e) {
        <div class="rf-field-error" role="alert">Not saved: {{ e }}</div>
      }
    </div>
  `,
  styles: `
    :host {
      display: flex;
      flex-direction: column;
      border: 1px solid var(--rf-border);
      border-radius: 12px;
      overflow: hidden;
      background: var(--rf-surface-1);
    }
    .preview {
      display: block;
      width: 100%;
      height: auto;
      aspect-ratio: 3 / 2;
      object-fit: cover;
      background: var(--rf-surface-3);
    }
    .preview.none {
      display: flex;
      align-items: center;
      justify-content: center;
      color: var(--rf-text-3);
      font-size: 13px;
    }
    .body {
      padding: 14px;
      display: flex;
      flex-direction: column;
      gap: 8px;
    }
    h2 {
      margin: 0;
      font-size: 16px;
    }
    .muted {
      color: var(--rf-text-2);
    }
    .small {
      font-size: 12px;
    }
    dl {
      margin: 0;
      display: grid;
      grid-template-columns: auto 1fr;
      gap: 4px 12px;
      font-size: 13px;
    }
    dt {
      color: var(--rf-text-2);
    }
    dd {
      margin: 0;
    }
    .note {
      margin: 0;
      font-size: 13px;
    }
    .note.ok {
      color: var(--rf-text-2);
    }
    .note.update {
      color: var(--rf-warn-text);
    }
    .actions {
      display: flex;
      flex-wrap: wrap;
      gap: 8px;
      margin-top: 4px;
    }
    .rf-btn {
      min-height: 44px;
      display: inline-flex;
      align-items: center;
      text-decoration: none;
    }
    .saving {
      display: flex;
      flex-direction: column;
      gap: 4px;
    }
    .bar {
      height: 6px;
      border-radius: 3px;
      background: var(--rf-surface-3);
      overflow: hidden;
    }
    .fill {
      height: 100%;
      background: var(--rf-ok);
    }
  `,
})
export class OfflinePackageCardComponent {
  protected readonly library = inject(OfflineLibrary);
  protected readonly f = OfflineRegionFormat;
  readonly region = input.required<OfflineRegion>();
  readonly kind = input<SavedPackageKind>('regions');
  /** A drawn area: what was asked for, kept with the saved package. */
  readonly request = input<OfflineAreaRequest | undefined>(undefined);

  protected readonly key = computed(() => OfflineLibrary.key(this.kind(), this.region().id));
  protected readonly saved = computed(() => this.library.saved().find((s) => s.key === this.key()));
  protected readonly outdated = computed(() => !!this.saved() && this.saved()!.region.version !== this.region().version);
  protected readonly progress = computed(() => this.library.saving()[this.key()]);
  protected readonly failure = computed(() => this.library.failures()[this.key()]);
  protected readonly percent = computed(() => {
    const p = this.progress();
    return p && p.total > 0 ? Math.min(100, Math.round((100 * p.received) / p.total)) : 0;
  });

  protected save(): void {
    void this.library.save(this.kind(), this.region(), this.request());
  }
}
