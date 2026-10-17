import { Component, ElementRef, OnInit, afterNextRender, computed, inject, signal, viewChild } from '@angular/core';
import { firstValueFrom } from 'rxjs';
import { ApiError, CredentialStore, Mission, MissionExporter, RfmapApiClient, Role, ShareLink } from '@rfmap/ui-kit';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { CompareService, FPV_CORE, FpvCoreOptions, fpvCoreData, fpvCoreOptions } from '../core/fpv-core';

interface LinkCard {
  role: Role;
  title: string;
  hint: string;
  link: ShareLink | null;
  url: string | null;
  /** Opens, expiry and pause of the link, from ShareLink */
  status: string[];
  paused: boolean;
}

const EXPIRY_CHOICES: { label: string; days: number | null }[] = [
  { label: 'Until revoked', days: null },
  { label: '1 day', days: 1 },
  { label: '7 days', days: 7 },
  { label: '30 days', days: 30 },
];

/** Share: edit link + view link, each with a 6-digit PIN; revoking needs the owner key; KML/GPX export. */
@Component({
  selector: 'fpv-share-dialog',
  template: `
    <div class="backdrop" (click)="close()"></div>
    <section #dialog class="dialog" role="dialog" aria-modal="true" aria-labelledby="share-title" tabindex="-1" (keydown.escape)="close()">
      <div class="head">
        <h2 id="share-title">Share “{{ ws.mission()?.name }}”</h2>
        <button type="button" class="rf-icon-btn" aria-label="Close share dialog" (click)="close()">✕</button>
      </div>
      <div class="rf-caption intro">No accounts. Anyone with a link and its PIN gets its access level; wrong PINs are rate-limited. Links are long random tokens and can be revoked at any time.</div>

      @if (!missionId()) {
        <div class="rf-card rf-card--plain">Calculate once to save the mission on the server; then you can share it.</div>
      } @else {
        @for (c of cards(); track c.role) {
          <div class="card">
            <div class="row"><b>{{ c.title }}</b><span class="rf-caption">{{ c.hint }}</span></div>
            @if (c.status.length) {
              <div class="rf-caption status" [class.paused]="c.paused">{{ c.status.join(' · ') }}</div>
            }
            @if (c.url) {
              <div class="row gap">
                <div class="url rf-mono" [title]="c.url">{{ c.url }}</div>
                <button type="button" class="rf-btn rf-btn--sm rf-btn--primary" (click)="copy(c.url, c.role)">{{ copied() === c.role ? 'Copied ✓' : 'Copy' }}</button>
              </div>
              <div class="row gap">
                <span class="rf-caption">PIN set when the link was made</span>
                <span class="grow"></span>
                <button type="button" class="rf-btn rf-btn--dense rf-btn--secondary" (click)="forget(c.role)">New link</button>
                <button type="button" class="rf-btn rf-btn--dense rf-btn--danger" (click)="revoke(c)" [disabled]="busy()">Revoke</button>
              </div>
            } @else if (c.link && !c.url) {
              <div class="rf-caption">A {{ c.role }} link exists ({{ c.link.label || c.link.linkId }}); its token was shown only when it was made.</div>
              <div class="row gap">
                <span class="grow"></span>
                <button type="button" class="rf-btn rf-btn--dense rf-btn--secondary" (click)="forget(c.role)">Make a new one</button>
                <button type="button" class="rf-btn rf-btn--dense rf-btn--danger" (click)="revoke(c)" [disabled]="busy()">Revoke</button>
              </div>
            } @else {
              <div class="row gap">
                <label class="rf-field grow">6-digit PIN for this link
                  <input class="rf-input rf-mono pin" inputmode="numeric" autocomplete="off" maxlength="6" [value]="pins()[c.role]" (input)="setPin(c.role, $any($event.target).value)" [class.is-invalid]="pinError()[c.role]" [attr.aria-invalid]="!!pinError()[c.role]" />
                </label>
                <button type="button" class="rf-btn rf-btn--sm rf-btn--secondary" (click)="randomPin(c.role)">Random</button>
              </div>
              <div class="row gap">
                <label class="rf-field grow">Link works
                  <select class="rf-input" [value]="expiry()[c.role]" (change)="setExpiry(c.role, +$any($event.target).value)">
                    @for (x of expiryChoices; track $index) {<option [value]="$index">{{ x.label }}</option>}
                  </select>
                </label>
                <button type="button" class="rf-btn rf-btn--sm rf-btn--primary create" (click)="create(c.role)" [disabled]="busy() || !ownerKey()">Create link</button>
              </div>
              @if (pinError()[c.role]; as e) {<div class="rf-field-error">{{ e }}</div>}
            }
          </div>
        }

        <div class="owner">
          <b>Owner key</b> · only this key can create or revoke links.
          @if (ownerKey()) {
            It is saved in this browser; download it to keep it safe.
            <div class="row gap top"><button type="button" class="rf-btn rf-btn--dense owner-btn" (click)="downloadKey()">Download owner key</button></div>
          } @else {
            This browser does not have it. Paste it to manage links.
            <div class="row gap top">
              <input class="rf-input rf-mono grow" aria-label="Owner key" [value]="keyInput()" (input)="keyInput.set($any($event.target).value)" />
              <button type="button" class="rf-btn rf-btn--dense owner-btn" (click)="saveKey()">Use key</button>
            </div>
          }
        </div>
      }
      @if (error(); as e) {<div class="rf-field-error" role="alert">{{ e }}</div>}
      <div class="row gap exports">
        <span class="rf-caption">Also export:</span>
        @if (cmp.available()) {
          <label class="sr-only" for="kml-alt">Heights in the export</label>
          <select id="kml-alt" class="rf-input alt" [value]="kmlAltitude()" (change)="kmlAltitude.set($any($event.target).value)">
            <option value="relativeToGround">Heights above ground</option>
            <option value="absolute">Heights above sea level</option>
          </select>
        }
        <button type="button" class="rf-btn rf-btn--dense rf-btn--secondary" [disabled]="exporting()" (click)="export('kml')">KML</button>
        <button type="button" class="rf-btn rf-btn--dense rf-btn--secondary" [disabled]="exporting()" (click)="export('gpx')">GPX</button>
      </div>
      <div class="rf-caption" role="status">{{ exportNote() }}</div>
    </section>
  `,
  styles: `
    :host { position: absolute; inset: 0; z-index: 20; display: flex; align-items: center; justify-content: center; }
    .backdrop { position: absolute; inset: 0; background: rgba(11, 15, 20, 0.6); }
    .dialog { position: relative; width: 480px; max-width: calc(100% - 32px); max-height: calc(100% - 32px); overflow-y: auto; padding: 20px; border-radius: 16px; background: var(--rf-surface-2); border: 1px solid var(--rf-border); display: flex; flex-direction: column; gap: 12px; }
    .head { display: flex; justify-content: space-between; align-items: center; }
    h2 { margin: 0; font-size: 18px; }
    .intro { line-height: 1.45; }
    .card { padding: 12px; border-radius: 12px; border: 1px solid var(--rf-border); background: var(--rf-surface-3); display: flex; flex-direction: column; gap: 10px; }
    .row { display: flex; justify-content: space-between; align-items: center; }
    .gap { gap: 8px; justify-content: flex-start; }
    .top { margin-top: 8px; }
    .grow { flex: 1; }
    .url { flex: 1; min-width: 0; height: 36px; display: flex; align-items: center; padding: 0 10px; border-radius: 8px; background: var(--rf-bg); border: 1px solid var(--rf-border); font-size: 12px; color: var(--rf-text-3); overflow: hidden; white-space: nowrap; text-overflow: ellipsis; }
    .pin { letter-spacing: 0.3em; font-size: 16px; }
    .owner { padding: 12px 14px; border-radius: 12px; border: 1px solid var(--rf-warn-border); background: var(--rf-warn-bg); font-size: 13px; line-height: 1.45; color: var(--rf-text-3); }
    .owner b { color: var(--rf-warn-text); }
    .owner-btn { border: 1px solid var(--rf-warn-border); color: var(--rf-warn-text); }
    .exports { padding-top: 4px; flex-wrap: wrap; }
    .alt { height: 32px; width: auto; }
    .status { color: var(--rf-text-2); }
    .status.paused { color: var(--rf-warn-text); }
    .create { align-self: flex-end; }
  `,
})
export class ShareDialogComponent implements OnInit {
  protected readonly ws = inject(FpvWorkspace);
  private readonly api = inject(RfmapApiClient);
  private readonly creds = inject(CredentialStore);
  protected readonly cmp = inject(CompareService);
  protected readonly kmlAltitude = signal<'relativeToGround' | 'absolute'>('relativeToGround');
  protected readonly exporting = signal(false);
  protected readonly exportNote = signal('');
  private readonly dialog = viewChild<ElementRef<HTMLElement>>('dialog');

  protected readonly missionId = this.ws.store.missionId;
  protected readonly links = signal<ShareLink[]>([]);
  /** Tokens are returned only on creation; keep them for this dialog's lifetime. */
  private readonly tokens = signal<Partial<Record<Role, { linkId: string; token: string }>>>({});
  private readonly hidden = signal<Partial<Record<Role, boolean>>>({});
  protected readonly pins = signal<Record<Role, string>>({ edit: '', view: '' });
  protected readonly pinError = signal<Partial<Record<Role, string>>>({});
  protected readonly busy = signal(false);
  protected readonly error = signal<string | null>(null);
  protected readonly copied = signal<Role | null>(null);
  protected readonly keyInput = signal('');
  protected readonly ownerKey = signal<string | null>(null);

  constructor() {
    afterNextRender(() => this.dialog()?.nativeElement.focus());
  }

  ngOnInit(): void {
    this.ownerKey.set(this.creds.ownerKey(this.missionId()));
    void this.refresh();
  }

  protected readonly expiryChoices = EXPIRY_CHOICES;
  /** Index into EXPIRY_CHOICES per role */
  protected readonly expiry = signal<Record<Role, number>>({ edit: 0, view: 0 });

  protected setExpiry(role: Role, i: number): void {
    this.expiry.update((e) => ({ ...e, [role]: i }));
  }

  private static fmt(iso: string): string {
    return new Date(iso).toLocaleString('en-GB', { day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' });
  }

  protected readonly cards = computed<LinkCard[]>(() => {
    const now = Date.now();
    const make = (role: Role, title: string, hint: string): LinkCard => {
      const tok = this.tokens()[role];
      const active = this.links().filter((l) => l.role === role && !l.revoked && !(l.expiresAt && Date.parse(l.expiresAt) <= now));
      const link = tok ? (active.find((l) => l.linkId === tok.linkId) ?? null) : this.hidden()[role] ? null : (active[active.length - 1] ?? null);
      const url = tok && link ? `${location.origin}/m/${role === 'edit' ? 'e' : 'v'}/${tok.token}` : null;
      const opened = link?.lastOpenedAt ? `last opened ${ShareDialogComponent.fmt(link.lastOpenedAt)}` : '';
      const status: string[] = [];
      let paused = false;
      if (link) {
        if (link.openCount !== undefined) status.push(`opened ${link.openCount} ${link.openCount === 1 ? 'time' : 'times'}`);
        status.push(link.expiresAt ? `expires ${ShareDialogComponent.fmt(link.expiresAt)}` : 'no expiry');
        if (link.pausedUntil && Date.parse(link.pausedUntil) > now) {
          paused = true;
          status.push(`paused after wrong PINs until ${ShareDialogComponent.fmt(link.pausedUntil)}`);
        }
      }
      return { role, title, hint: opened || hint, link, url, status, paused };
    };
    return [make('edit', 'Can edit', 'PIN required · revoke with owner key'), make('view', 'View only', 'PIN required · opens on phone')];
  });

  private async refresh(): Promise<void> {
    const id = this.missionId();
    const key = this.ownerKey();
    if (!id || !key) return;
    try {
      this.links.set(await this.api.listLinks(id, key));
    } catch (e) {
      this.error.set(ApiError.from(e).status === 403 ? 'The owner key was not accepted.' : ApiError.from(e).message);
    }
  }

  protected setPin(role: Role, v: string): void {
    this.pins.update((p) => ({ ...p, [role]: v.replace(/\D/g, '').slice(0, 6) }));
    this.pinError.update((e) => ({ ...e, [role]: undefined }));
  }

  protected randomPin(role: Role): void {
    const n = crypto.getRandomValues(new Uint32Array(1))[0] % 1_000_000;
    this.setPin(role, String(n).padStart(6, '0'));
  }

  protected async create(role: Role): Promise<void> {
    const id = this.missionId();
    const pin = this.pins()[role];
    if (!/^[0-9]{6}$/.test(pin)) {
      this.pinError.update((e) => ({ ...e, [role]: 'The PIN must be exactly 6 digits.' }));
      return;
    }
    if (!id) return;
    this.busy.set(true);
    this.error.set(null);
    try {
      const days = EXPIRY_CHOICES[this.expiry()[role]]?.days ?? null;
      const expiresAt = days ? new Date(Date.now() + days * 86400e3).toISOString() : undefined;
      const req = { role, pin, label: role === 'edit' ? 'Edit link' : 'View link', ...(expiresAt ? { expiresAt } : {}) };
      const created = await this.api.createLink(id, req, this.ownerKey() ?? undefined);
      if (created.token) this.tokens.update((t) => ({ ...t, [role]: { linkId: created.linkId, token: created.token! } }));
      this.hidden.update((h) => ({ ...h, [role]: false }));
      await this.refresh();
      if (!this.links().some((l) => l.linkId === created.linkId)) this.links.update((l) => [...l, created]);
    } catch (e) {
      this.error.set(ApiError.from(e).message);
    } finally {
      this.busy.set(false);
    }
  }

  protected async revoke(c: LinkCard): Promise<void> {
    const id = this.missionId();
    if (!id || !c.link) return;
    if (!this.ownerKey()) {
      this.error.set('Revoking needs the owner key.');
      return;
    }
    this.busy.set(true);
    try {
      await this.api.revokeLink(id, c.link.linkId, this.ownerKey() ?? undefined);
      this.tokens.update((t) => ({ ...t, [c.role]: undefined }));
      await this.refresh();
    } catch (e) {
      this.error.set(ApiError.from(e).message);
    } finally {
      this.busy.set(false);
    }
  }

  /** "New link": show the create form (the old link stays valid until revoked). */
  protected forget(role: Role): void {
    this.tokens.update((t) => ({ ...t, [role]: undefined }));
    this.hidden.update((h) => ({ ...h, [role]: true }));
    this.setPin(role, '');
  }

  protected async copy(text: string, role: Role): Promise<void> {
    try {
      await navigator.clipboard.writeText(text);
      this.copied.set(role);
      setTimeout(() => this.copied.set(null), 2000);
    } catch {
      this.error.set('Copy is blocked in this browser; select the link and copy it.');
    }
  }

  protected saveKey(): void {
    const id = this.missionId();
    const key = this.keyInput().trim();
    if (!id || !key) return;
    this.creds.setOwnerKey(id, key);
    this.ownerKey.set(key);
    void this.refresh();
  }

  protected downloadKey(): void {
    const id = this.missionId();
    const key = this.ownerKey();
    if (!id || !key) return;
    MissionExporter.download({ filename: `${id}-owner-key.txt`, mime: 'text/plain', content: `FPV Radio Map owner key\nmission: ${id}\nkey: ${key}\n` });
  }

  /**
   * Feature 14: the server exports through fpv.core (options.export, kmlAltitude, exportStepM) and
   * returns the files in data.export; without fpv.core the browser writes them itself (heights above ground only).
   */
  protected async export(kind: 'kml' | 'gpx'): Promise<void> {
    const m = this.ws.mission();
    if (!m) return;
    if (!this.cmp.available()) {
      const ex = new MissionExporter();
      MissionExporter.download(kind === 'kml' ? ex.toKml(m, this.ws.store.result()) : ex.toGpx(m));
      this.exportNote.set('Exported in the browser (fpv.core is not installed): heights above ground only.');
      return;
    }
    this.exporting.set(true);
    this.exportNote.set(`Preparing ${kind.toUpperCase()} on the server…`);
    try {
      const { compare: _skip, ...opts } = fpvCoreOptions(m);
      const options: FpvCoreOptions = { ...opts, export: [kind], kmlAltitude: this.kmlAltitude(), exportStepM: opts.exportStepM ?? 50 };
      const req: Mission = { ...m, modules: { ...(m.modules ?? {}), [FPV_CORE]: options as Record<string, unknown> } };
      const result = await firstValueFrom(this.api.calcRoute(this.ws.store.missionId(), req));
      const file = fpvCoreData(result)?.export?.[kind];
      if (!file?.content) throw new Error(`The server returned no ${kind.toUpperCase()} file.`);
      const ex = new MissionExporter();
      const name = (kind === 'kml' ? ex.toKml(m, null) : ex.toGpx(m)).filename;
      MissionExporter.download({ filename: name, mime: file.mediaType || (kind === 'kml' ? 'application/vnd.google-earth.kml+xml' : 'application/gpx+xml'), content: file.content });
      this.exportNote.set(`${kind.toUpperCase()} exported by fpv.core, ${this.kmlAltitude() === 'absolute' ? 'heights above sea level' : 'heights above ground'}.`);
    } catch (e) {
      this.exportNote.set(`Export failed: ${ApiError.from(e).message}`);
    } finally {
      this.exporting.set(false);
    }
  }

  protected close(): void {
    this.ws.shareOpen.set(false);
  }
}
