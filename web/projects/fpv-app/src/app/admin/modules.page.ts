import { Component, OnInit, computed, effect, inject, signal, untracked } from '@angular/core';
import { ApiError, CredentialStore, ModuleInfo, ModuleUiLoader, RfmapApiClient, TrustedKey, ValidationReport } from '@rfmap/ui-kit';

/** Module manager (ModuleManager.dc.html): installed modules with lifecycle, upload + validation report. */
@Component({
  selector: 'fpv-modules-page',
  template: `
    <div class="layout">
      <main class="main">
        <div class="title-row">
          <h1>Installed modules</h1>
          <span class="muted">{{ modules().length }}</span>
          <span class="grow"></span>
          @if (deprecatedCount()) {<span class="warn-text">{{ deprecatedCount() }} deprecated</span>}
        </div>
        <div class="table" role="table" aria-label="Installed modules">
          <div class="tr th" role="row">
            <span role="columnheader">Module</span><span role="columnheader">Version</span><span role="columnheader">Stage</span><span role="columnheader">Requires</span><span role="columnheader">Notes</span><span role="columnheader"><span class="sr-only">Actions</span></span>
          </div>
          @for (m of modules(); track m.id + '@' + m.version) {
            <div class="tr" role="row" [class.dep]="m.lifecycle === 'deprecated'">
              <span role="cell" class="rf-mono">{{ m.id }}</span>
              <span role="cell" class="rf-mono">{{ m.version }}</span>
              <span role="cell"><span [class]="'rf-badge rf-badge--' + (m.lifecycle ?? 'stable')">{{ m.lifecycle ?? '—' }}</span>
                @if (m.state && m.state !== 'active') {<span class="rf-badge rf-badge--failed state">{{ m.state }}</span>}
              </span>
              <span role="cell" class="muted">{{ m.requires?.join(', ') || '—' }}</span>
              <span role="cell" [class.warn-text]="m.lifecycle === 'deprecated'" class="notes">{{ notes(m) }}</span>
              <span role="cell" class="acts">
                @if (isAdmin() && m.id) {
                  <button type="button" class="rf-btn rf-btn--dense rf-btn--secondary" [disabled]="busy()" (click)="toggle(m)" [attr.aria-label]="(m.state === 'disabled' ? 'Enable ' : 'Disable ') + m.id">{{ m.state === 'disabled' ? 'Enable' : 'Disable' }}</button>
                  @if (!m.builtIn) {
                    <button type="button" class="rf-btn rf-btn--dense rf-btn--danger" [disabled]="busy()" (click)="remove(m)" [attr.aria-label]="'Remove ' + m.id">Remove</button>
                  }
                }
              </span>
            </div>
          }
        </div>
        <section class="lifecycle">
          <b>Lifecycle</b>
          <div class="chain">
            <span class="rf-badge rf-badge--alpha">alpha · admin only</span><span class="arrow">→</span>
            <span class="rf-badge rf-badge--beta">beta · opt-in, labelled</span><span class="arrow">→</span>
            <span class="rf-badge rf-badge--stable">stable · API frozen in major</span><span class="arrow">→</span>
            <span class="rf-badge rf-badge--deprecated">deprecated · works, warns</span><span class="arrow">→</span>
            <span class="rf-badge rf-badge--removed">removed</span>
          </div>
          <div class="muted small">A deprecated API stays for at least 3 minor releases. The warning names the version it goes away in and the replacement, and appears in the upload report, server logs and a banner in affected missions.</div>
        </section>
        @if (notices().length) {
          <section class="lifecycle">
            <b>Load-time notices</b>
            @for (n of notices(); track $index) {<div class="small warn-text">{{ noticeText(n) }}</div>}
          </section>
        }
        @if (uiStatus().length) {
          <section class="lifecycle">
            <b>Module UI bundles in this browser</b>
            @for (s of uiStatus(); track s.moduleId) {
              <div class="small"><code>{{ s.moduleId }}</code> · {{ s.state }}{{ s.state === 'failed' ? ': ' + s.error : s.state === 'skipped' ? ': ' + s.reason : s.state === 'loaded' ? ' · ' + s.contributions + ' contribution(s)' : '' }}</div>
            }
          </section>
        }
      </main>
      <aside class="side">
        <label class="drop" [class.over]="over()" (dragover)="$event.preventDefault(); over.set(true)" (dragleave)="over.set(false)" (drop)="onDrop($event)">
          <b>Drop a module package</b><span class="small">.rfmod · signed · max 50 MB · or click to choose</span>
          <input type="file" accept=".rfmod,application/octet-stream" class="sr-only" (change)="onPick($event)" />
        </label>
        @if (busy()) {<div class="muted" role="status">Checking {{ fileName() }}…</div>}
        @if (error(); as e) {<div class="rf-field-error" role="alert">{{ e }}</div>}
        @if (report(); as r) {
          <div class="report-head"><b>{{ fileName() }}</b><span class="small muted">checked {{ checkedAgo() }}</span></div>
          <ul class="checks">
            @for (c of r.checks ?? []; track $index) {
              <li [class]="checkClass(c)">
                <span class="mark" aria-hidden="true">{{ checkMark(c) }}</span>
                <span><span class="sr-only">{{ c.ok ? (isWarning(c) ? 'Warning:' : 'Passed:') : 'Failed:' }}</span> <b>{{ c.name }}</b> · {{ c.detail }}</span>
              </li>
            }
          </ul>
          <div class="actions">
            <button type="button" class="rf-btn rf-btn--primary" [disabled]="!r.ok || busy()" (click)="install()">{{ r.ok ? 'Install as beta' : 'Cannot install' }}</button>
            <button type="button" class="rf-btn rf-btn--danger" (click)="reject()">Reject</button>
          </div>
          @if (installed()) {<div class="ok-text" role="status">Installed. It appears in the list after the server reloads its modules.</div>}
        }
        <section class="keys" aria-labelledby="keys-title">
          <h2 id="keys-title">Trusted signing keys</h2>
          @if (!isAdmin()) {
            <div class="muted small">Set the admin token (top right) to see and change the keys.</div>
          } @else {
            <div class="muted small">Only packages signed with one of these keys load. Removing a key stops its modules at once; the built-in owner key stays.</div>
            @for (k of keys(); track k.id) {
              <div class="key">
                <div class="key-text">
                  <b>{{ k.label || k.id }}</b>
                  @if (k.builtIn) {<span class="rf-badge rf-badge--stable">built in</span>}
                  <span class="rf-mono small muted">{{ k.id }}{{ k.key ? ' · ' + k.key : '' }}</span>
                </div>
                @if (!k.builtIn && k.id) {
                  <button type="button" class="rf-btn rf-btn--dense rf-btn--danger" [disabled]="busy()" (click)="deleteKey(k)" [attr.aria-label]="'Remove key ' + (k.label || k.id)">Remove</button>
                }
              </div>
            }
            <label class="rf-field">Public key (rfmod text form)
              <textarea class="rf-input rf-mono keyin" rows="2" [value]="newKey()" (input)="newKey.set($any($event.target).value)"></textarea>
            </label>
            <label class="rf-field">Label
              <input class="rf-input" [value]="newLabel()" (input)="newLabel.set($any($event.target).value)" />
            </label>
            <button type="button" class="rf-btn rf-btn--secondary" [disabled]="busy() || !newKey().trim()" (click)="addKey()">Add key</button>
            @if (keyError(); as e) {<div class="rf-field-error" role="alert">{{ e }}</div>}
          }
        </section>
      </aside>
    </div>
  `,
  styles: `
    .layout { display: flex; flex-wrap: wrap; min-height: 100%; }
    .main { flex: 999 1 640px; min-width: 0; padding: 18px 20px; display: flex; flex-direction: column; gap: 12px; }
    .side { flex: 1 1 380px; max-width: 480px; border-left: 1px solid var(--rf-border); background: var(--rf-surface-2); padding: 18px; display: flex; flex-direction: column; gap: 12px; }
    .title-row { display: flex; align-items: center; gap: 10px; }
    h1 { margin: 0; font-size: 20px; }
    .grow { flex: 1; }
    .muted { color: var(--rf-text-2); }
    .small { font-size: 12px; }
    .warn-text { color: var(--rf-warn-text); font-size: 13px; }
    .ok-text { color: var(--rf-ok-text); font-size: 13px; }
    .table { border: 1px solid var(--rf-border); border-radius: 12px; overflow: hidden; }
    .tr { display: grid; grid-template-columns: 1.8fr 0.8fr 1.2fr 1.2fr 2.2fr 1.5fr; border-top: 1px solid var(--rf-divider); align-items: center; }
    .tr > span { padding: 10px 12px; }
    .tr.th { background: var(--rf-surface-3); font-size: 12px; color: var(--rf-text-2); font-weight: 600; border-top: 0; }
    .tr.dep { background: var(--rf-warn-bg); }
    .state { margin-left: 4px; }
    .notes { font-size: 13px; }
    .lifecycle { padding: 14px; border-radius: 12px; border: 1px solid var(--rf-border); background: var(--rf-surface-1); display: flex; flex-direction: column; gap: 10px; }
    .chain { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
    .arrow { color: var(--rf-border-strong); }
    .drop { min-height: 92px; border: 2px dashed var(--rf-border-strong); border-radius: 12px; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 4px; color: var(--rf-text-2); cursor: pointer; text-align: center; }
    .drop b { color: var(--rf-text); }
    .drop.over, .drop:focus-within { border-color: var(--rf-ok); }
    .report-head { display: flex; justify-content: space-between; align-items: baseline; font-size: 16px; }
    .checks { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 6px; font-size: 13px; }
    .checks li { display: flex; gap: 8px; padding: 2px 0; }
    .checks .mark { width: 16px; flex: none; }
    .checks .ok .mark { color: var(--rf-ok-text); }
    .checks .bad { color: var(--rf-blocked-text); }
    .checks .warn { padding: 8px; border-radius: 8px; background: var(--rf-warn-bg); color: var(--rf-warn-text); }
    .acts { display: flex; gap: 6px; justify-content: flex-end; flex-wrap: wrap; }
    .keys { display: flex; flex-direction: column; gap: 8px; padding-top: 12px; border-top: 1px solid var(--rf-divider); }
    h2 { margin: 0; font-size: 16px; }
    .key { display: flex; gap: 8px; align-items: center; justify-content: space-between; padding: 8px 10px; border-radius: 10px; border: 1px solid var(--rf-border); background: var(--rf-surface-1); }
    .key-text { display: flex; flex-wrap: wrap; gap: 6px; align-items: baseline; min-width: 0; }
    .key-text .rf-mono { overflow-wrap: anywhere; }
    .keyin { height: auto; min-height: 56px; padding: 8px; font-size: 12px; }
    .actions { display: flex; gap: 8px; }
    .actions .rf-btn--primary { flex: 1; }
  `,
})
export class ModulesPage implements OnInit {
  private readonly api = inject(RfmapApiClient);
  private readonly loader = inject(ModuleUiLoader);
  private readonly creds = inject(CredentialStore);
  protected readonly isAdmin = computed(() => !!this.creds.adminTokenSignal());
  protected readonly keys = signal<TrustedKey[]>([]);
  protected readonly notices = signal<Record<string, unknown>[]>([]);
  protected readonly newKey = signal('');
  protected readonly newLabel = signal('');
  protected readonly keyError = signal<string | null>(null);
  protected readonly modules = signal<ModuleInfo[]>([]);
  protected readonly report = signal<ValidationReport | null>(null);
  protected readonly fileName = signal('');
  protected readonly busy = signal(false);
  protected readonly error = signal<string | null>(null);
  protected readonly over = signal(false);
  protected readonly installed = signal(false);
  private checkedAt = 0;
  private file: File | null = null;
  protected readonly uiStatus = this.loader.status;

  protected readonly deprecatedCount = computed(() => this.modules().filter((m) => m.lifecycle === 'deprecated').length);

  async ngOnInit(): Promise<void> {
    try {
      this.modules.set(await this.api.modules());
    } catch (e) {
      this.error.set(ApiError.from(e).message);
    }
  }

  constructor() {
    // keys and notices need the admin token; load them whenever it is set
    effect(() => {
      if (this.isAdmin()) untracked(() => void this.loadAdmin());
    });
  }

  private async loadAdmin(): Promise<void> {
    try {
      this.keys.set(await this.api.adminKeys());
      this.notices.set(await this.api.moduleNotices());
    } catch (e) {
      this.keyError.set(ApiError.from(e).message);
    }
  }

  protected noticeText(n: Record<string, unknown>): string {
    const parts = ['module', 'message', 'detail', 'text'].map((k) => n[k]).filter((v) => typeof v === 'string');
    return parts.length ? parts.join(': ') : JSON.stringify(n);
  }

  protected async toggle(m: ModuleInfo): Promise<void> {
    await this.adminCall(async () => this.modules.set(await this.api.setModule(m.id!, { enabled: m.state === 'disabled' })));
  }

  protected async remove(m: ModuleInfo): Promise<void> {
    await this.adminCall(async () => {
      await this.api.removeModule(m.id!);
      this.modules.set(await this.api.modules());
    });
  }

  protected async addKey(): Promise<void> {
    this.keyError.set(null);
    await this.adminCall(async () => {
      this.keys.set(await this.api.addKey(this.newKey().trim(), this.newLabel().trim() || undefined));
      this.newKey.set('');
      this.newLabel.set('');
    }, this.keyError);
  }

  protected async deleteKey(k: TrustedKey): Promise<void> {
    this.keyError.set(null);
    await this.adminCall(async () => {
      await this.api.deleteKey(k.id!);
      this.keys.set(await this.api.adminKeys());
    }, this.keyError);
  }

  private async adminCall(fn: () => Promise<void>, errSignal = this.error): Promise<void> {
    this.busy.set(true);
    try {
      await fn();
    } catch (e) {
      const err = ApiError.from(e);
      errSignal.set(err.status === 401 ? 'Set the admin token first (top right).' : err.message);
    } finally {
      this.busy.set(false);
    }
  }

  protected notes(m: ModuleInfo): string {
    if (m.deprecation) {
      const d = m.deprecation;
      return [d.since ? `Since ${d.since}` : '', d.removedIn ? `removed in ${d.removedIn}` : '', d.message ?? (d.replacement ? `Use ${d.replacement}.` : '')].filter(Boolean).join(', ').replace(', Use', '. Use');
    }
    if (m.stateDetail) return m.stateDetail;
    if (m.builtIn) return 'Built in · can be disabled, not removed';
    if (m.lifecycle === 'beta') return 'Users see a beta label';
    if (m.lifecycle === 'alpha') return 'Hidden from users';
    return m.ui?.bundle ? `UI bundle · slots ${m.ui.slots?.join(', ')}` : '';
  }

  protected isWarning(c: { detail?: string }): boolean {
    return (c.detail ?? '').toLowerCase().startsWith('warning');
  }
  protected checkClass(c: { ok?: boolean; detail?: string }): string {
    return !c.ok ? 'bad' : this.isWarning(c) ? 'warn' : 'ok';
  }
  protected checkMark(c: { ok?: boolean; detail?: string }): string {
    return !c.ok ? '✕' : this.isWarning(c) ? '!' : '✓';
  }
  protected checkedAgo(): string {
    const s = Math.max(1, Math.round((Date.now() - this.checkedAt) / 1000));
    return `${s} s ago`;
  }

  protected onPick(e: Event): void {
    const f = (e.target as HTMLInputElement).files?.[0];
    if (f) void this.check(f);
  }
  protected onDrop(e: DragEvent): void {
    e.preventDefault();
    this.over.set(false);
    const f = e.dataTransfer?.files?.[0];
    if (f) void this.check(f);
  }

  private async check(f: File): Promise<void> {
    this.file = f;
    this.fileName.set(f.name);
    this.report.set(null);
    this.installed.set(false);
    this.error.set(null);
    if (f.size > 50 * 1024 * 1024) {
      this.error.set('The package is larger than 50 MB.');
      return;
    }
    this.busy.set(true);
    try {
      this.report.set(await this.api.uploadModule(f, false));
      this.checkedAt = Date.now();
    } catch (e) {
      const err = ApiError.from(e);
      this.error.set(err.status === 401 ? 'Set the admin token first (top right).' : err.message);
    } finally {
      this.busy.set(false);
    }
  }

  protected async install(): Promise<void> {
    if (!this.file) return;
    this.busy.set(true);
    try {
      const r = await this.api.uploadModule(this.file, true);
      this.report.set(r);
      this.installed.set(!!r.ok);
    } catch (e) {
      this.error.set(ApiError.from(e).message);
    } finally {
      this.busy.set(false);
    }
  }

  protected reject(): void {
    this.file = null;
    this.report.set(null);
    this.fileName.set('');
  }
}
