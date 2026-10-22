import { Component, OnInit, computed, effect, inject, signal, untracked } from '@angular/core';
import { ApiError, CredentialStore, ModuleInfo, ModuleUiLoader, RfmapApiClient, TrustedKey } from '@rfmap/ui-kit';

/**
 * Module manager (ModuleManager.dc.html): the installed modules with their lifecycle, enable and
 * configure, and a reload after the operator installed or removed one on the server machine with
 * the rfmod CLI. Nothing is uploaded through the browser: the API has no route for it.
 */
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
          <div class="muted small">A deprecated API stays for at least 3 minor releases. The warning names the version it goes away in and the replacement, and appears in the install report, server logs and a banner in affected missions.</div>
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
        <section class="install" aria-labelledby="install-title">
          <h2 id="install-title">Install or remove a module</h2>
          <div class="muted small">Modules are installed on the server machine, never uploaded here. The <code>rfmod</code> CLI verifies the signature, checks the dependencies and runs the self-test in a sandboxed worker before anything is installed.</div>
          <pre class="rf-mono cli">rfmod install FILE.rfmod --root /var/lib/rfmap/modules
rfmod remove MODULE_ID --root /var/lib/rfmap/modules
rfmod list --root /var/lib/rfmap/modules</pre>
          <div class="muted small">Then reload, so the running server picks the change up.</div>
          <div class="actions">
            <button type="button" class="rf-btn rf-btn--primary" [disabled]="!isAdmin() || busy()" (click)="reload()">Reload modules</button>
          </div>
          @if (busy()) {<div class="muted" role="status">Reloading…</div>}
          @if (error(); as e) {<div class="rf-field-error" role="alert">{{ e }}</div>}
          @if (reloadedAt()) {<div class="ok-text" role="status">Reloaded {{ reloadedAgo() }}: {{ modules().length }} modules, {{ notices().length }} notice(s).</div>}
        </section>
        <section class="keys" aria-labelledby="keys-title">
          <h2 id="keys-title">Trusted signing keys</h2>
          @if (!isAdmin()) {
            <div class="muted small">Set the admin token (top right) to see the keys.</div>
          } @else {
            <div class="muted small">Only packages signed with one of these keys load. Keys are added and removed on the server machine, then reloaded here; the built-in owner key stays.</div>
            @for (k of keys(); track k.id) {
              <div class="key">
                <div class="key-text">
                  <b>{{ k.label || k.id }}</b>
                  @if (k.builtIn) {<span class="rf-badge rf-badge--stable">built in</span>}
                  <span class="rf-mono small muted">{{ k.id }}{{ k.key ? ' · ' + k.key : '' }}</span>
                </div>
              </div>
            }
            <pre class="rf-mono cli">rfmod trust add AUTHOR.pub --label "Author" --trust-store /var/lib/rfmap/keys.json
rfmod trust remove KEY_ID --trust-store /var/lib/rfmap/keys.json</pre>
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
    .acts { display: flex; gap: 6px; justify-content: flex-end; flex-wrap: wrap; }
    .install { display: flex; flex-direction: column; gap: 8px; }
    .cli { margin: 0; padding: 10px; border-radius: 10px; border: 1px solid var(--rf-border); background: var(--rf-surface-1); font-size: 12px; white-space: pre-wrap; overflow-wrap: anywhere; }
    .keys { display: flex; flex-direction: column; gap: 8px; padding-top: 12px; border-top: 1px solid var(--rf-divider); }
    h2 { margin: 0; font-size: 16px; }
    .key { display: flex; gap: 8px; align-items: center; justify-content: space-between; padding: 8px 10px; border-radius: 10px; border: 1px solid var(--rf-border); background: var(--rf-surface-1); }
    .key-text { display: flex; flex-wrap: wrap; gap: 6px; align-items: baseline; min-width: 0; }
    .key-text .rf-mono { overflow-wrap: anywhere; }
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
  protected readonly keyError = signal<string | null>(null);
  protected readonly modules = signal<ModuleInfo[]>([]);
  protected readonly busy = signal(false);
  protected readonly error = signal<string | null>(null);
  protected readonly reloadedAt = signal(0);
  protected readonly uiStatus = this.loader.status;

  protected readonly deprecatedCount = computed(() => this.modules().filter((m) => m.lifecycle === 'deprecated').length);

  ngOnInit(): void {
    this.loadModules().catch((e) => this.error.set(ApiError.from(e).message));
  }

  private async loadModules(): Promise<void> {
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
    this.error.set(null);
    await this.adminCall(async () => this.modules.set(await this.api.setModule(m.id!, { enabled: m.state === 'disabled' })));
  }

  /** The server re-reads its module root and trust store; the listing and notices follow. */
  protected async reload(): Promise<void> {
    this.error.set(null);
    await this.adminCall(async () => {
      const r = await this.api.reloadModules();
      this.modules.set(r.modules ?? (await this.api.modules()));
      this.notices.set((r.notices ?? []).map((text) => ({ text })));
      this.keys.set(await this.api.adminKeys());
      this.reloadedAt.set(Date.now());
    });
  }

  protected reloadedAgo(): string {
    const s = Math.max(1, Math.round((Date.now() - this.reloadedAt()) / 1000));
    return `${s} s ago`;
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
}
