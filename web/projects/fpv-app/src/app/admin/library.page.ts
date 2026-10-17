import { Component, OnInit, computed, inject, signal } from '@angular/core';
import { ApiError, CredentialStore, LIBRARY_KINDS, LibraryKind, MissionExporter, RfmapApiClient, SchemaValidator } from '@rfmap/ui-kit';
import { FpvDataService } from '../core/fpv-data.service';

type Entry = Record<string, unknown> & { id?: string; name?: string; verified?: boolean; version?: number };

/** Reference library editor (Library.dc.html): list per kind, JSON editor validated live with the contract schema. */
@Component({
  selector: 'fpv-library-page',
  template: `
    <div class="layout">
      <nav class="kinds" aria-label="Library sections">
        @for (k of kinds; track k.kind) {
          <button type="button" class="kind" [class.on]="k.kind === kind()" [attr.aria-pressed]="k.kind === kind()" (click)="selectKind(k.kind)">
            <span>{{ k.title }}</span><span class="count">{{ k.kind === kind() ? entries().length : '' }}</span>
          </button>
        }
        <div class="io">
          <label class="rf-btn rf-btn--secondary rf-btn--dense">Import JSON<input type="file" accept="application/json,.json" class="sr-only" (change)="importFile($event)" /></label>
          <button type="button" class="rf-btn rf-btn--secondary rf-btn--dense" (click)="exportAll()">Export JSON</button>
        </div>
      </nav>
      <section class="list" aria-label="Entries">
        @if (isAdmin()) {
          <button type="button" class="rf-btn rf-btn--secondary new" (click)="newEntry()">+ New</button>
        }
        @for (e of entries(); track e.id) {
          <button type="button" class="item" [class.on]="e.id === selectedId()" [class.unverified]="!e.verified" (click)="select(e)">
            <span class="item-head"><b>{{ e.name ?? e.id }}</b>
              <span [class]="e.verified ? 'vbadge ok' : 'vbadge warn'"><span aria-hidden="true">{{ e.verified ? '✓' : '!' }}</span> {{ e.verified ? 'verified' : 'unverified' }}</span>
            </span>
            <span class="small muted">{{ summary(e) }}</span>
            <span class="small src">{{ sourceText(e) }}</span>
          </button>
        } @empty {
          <div class="muted">No entries yet.</div>
        }
      </section>
      <section class="editor" aria-label="Entry editor">
        <div class="head">
          <div>
            <h1>{{ draftName() }}</h1>
            <div class="small muted">{{ selectedId() ? 'rev ' + (draftVersion() ?? '—') : 'New entry' }} · schema {{ schemaFile() }}</div>
          </div>
          <span class="grow"></span>
          @if (isAdmin()) {
            <button type="button" class="rf-btn rf-btn--primary" [disabled]="!validation().ok || busy()" (click)="save()">{{ busy() ? 'Saving…' : 'Save' }}</button>
          }
        </div>
        <div class="prov" [class.unverified]="!draftVerified()">
          <b>{{ draftVerified() ? '✓ Verified' : '! Unverified' }}</b>
          <span>{{ draftVerified() ? 'An admin checked the numbers against the source.' : 'Not checked by an admin yet; results that use it are only as good as the source.' }}</span>
          <span class="small">Source: {{ draftSource() || 'none given' }}</span>
        </div>
        @if (!isAdmin()) {
          <div class="note small" role="note">Read only. Only an admin can add or change library entries, and only an admin marks an entry verified. Set the admin token at the top right to edit.</div>
        }
        <label class="rf-caption" for="lib-json">Entry JSON</label>
        <textarea id="lib-json" class="json rf-mono" spellcheck="false" [readOnly]="!isAdmin()" [value]="text()" (input)="text.set($any($event.target).value)" aria-describedby="lib-issues"></textarea>
        <div id="lib-issues" class="issues" role="status" aria-live="polite">
          @if (parseError(); as p) {
            <div class="bad"><span aria-hidden="true">✕</span> Not valid JSON: {{ p }}</div>
          } @else if (validation().ok) {
            <div class="ok"><span aria-hidden="true">✓</span> Matches the {{ schemaFile() }} schema.</div>
          } @else {
            @for (i of validation().issues; track $index) {
              <div class="bad"><span aria-hidden="true">✕</span> <code>{{ i.path || '/' }}</code> {{ i.message }}</div>
            }
          }
          @if (serverIssues().length) {
            <div class="small muted">Server report:</div>
            @for (i of serverIssues(); track $index) {<div class="bad"><span aria-hidden="true">✕</span> <code>{{ i.path || '/' }}</code> {{ i.message }}</div>}
          }
          @if (message(); as m) {<div [class]="m.ok ? 'ok' : 'bad'">{{ m.text }}</div>}
          @if (conflict()) {
            <div><button type="button" class="rf-btn rf-btn--dense rf-btn--secondary" (click)="loadCurrent()">Load the current version</button> <span class="small muted">Loading replaces this draft; copy your change first.</span></div>
          }
        </div>
        @if (kind() === 'radio-profiles') {
        <div class="formula">
          <b>Formula · in code, not editable</b>
          <div class="rf-mono small">S = 10·log₁₀(k·T₀·B) + 30 + NF + SNR &nbsp; k = 1.380649·10⁻²³ J/K, T₀ = 290 K</div>
        </div>
        }
      </section>
    </div>
  `,
  styles: `
    .layout { display: grid; grid-template-columns: 220px 300px minmax(0, 1fr); min-height: 100%; }
    .kinds { border-right: 1px solid var(--rf-border); padding: 14px 10px; display: flex; flex-direction: column; gap: 4px; background: var(--rf-surface-1); }
    .kind { height: 40px; border: 0; border-radius: 8px; background: transparent; color: var(--rf-text-2); text-align: left; padding: 0 10px; display: flex; justify-content: space-between; align-items: center; font: inherit; font-weight: 600; cursor: pointer; }
    .kind.on { background: var(--rf-selected); color: var(--rf-text); }
    .count { color: var(--rf-text-2); font-weight: 400; }
    .io { margin-top: auto; display: flex; flex-direction: column; gap: 6px; padding-top: 16px; }
    .list { border-right: 1px solid var(--rf-border); padding: 14px; display: flex; flex-direction: column; gap: 8px; overflow-y: auto; }
    .item { border: 1px solid var(--rf-border); border-radius: 12px; background: var(--rf-surface-2); color: var(--rf-text); padding: 10px 12px; text-align: left; display: flex; flex-direction: column; gap: 4px; font: inherit; cursor: pointer; }
    .item.on { border-color: var(--rf-ok); background: var(--rf-selected); }
    .item-head { display: flex; justify-content: space-between; gap: 8px; align-items: baseline; }
    .vbadge { font-size: 11px; padding: 1px 6px; border-radius: 6px; white-space: nowrap; flex: none; }
    .vbadge.ok { background: var(--rf-ok-bg); color: var(--rf-ok-text); }
    .vbadge.warn { background: var(--rf-warn-bg); color: var(--rf-warn-text); }
    .src { color: var(--rf-text-3); }
    .prov { display: flex; flex-direction: column; gap: 2px; padding: 10px 12px; border-radius: 10px; background: var(--rf-ok-bg); font-size: 13px; }
    .prov b { color: var(--rf-ok-text); }
    .prov.unverified { background: var(--rf-warn-bg); }
    .prov.unverified b { color: var(--rf-warn-text); }
    .note { padding: 8px 10px; border-radius: 8px; border: 1px solid var(--rf-border); color: var(--rf-text-2); }
    .editor { padding: 18px 20px; display: flex; flex-direction: column; gap: 10px; min-width: 0; }
    .head { display: flex; align-items: flex-start; gap: 10px; }
    h1 { margin: 0; font-size: 20px; }
    .grow { flex: 1; }
    .json { min-height: 340px; flex: 1; resize: vertical; padding: 12px; border-radius: 8px; border: 1px solid var(--rf-border); background: var(--rf-bg); color: var(--rf-text); font-size: 13px; line-height: 1.5; }
    .issues { display: flex; flex-direction: column; gap: 4px; font-size: 13px; }
    .ok { color: var(--rf-ok-text); }
    .bad { color: var(--rf-blocked-text); }
    .muted { color: var(--rf-text-2); }
    .small { font-size: 12px; }
    .formula { padding: 12px; border: 1px solid var(--rf-border); border-radius: 12px; background: var(--rf-surface-1); display: flex; flex-direction: column; gap: 6px; }
  `,
})
export class LibraryPage implements OnInit {
  private readonly api = inject(RfmapApiClient);
  private readonly data = inject(FpvDataService);
  private readonly creds = inject(CredentialStore);
  private readonly validator = new SchemaValidator();
  protected readonly kinds = LIBRARY_KINDS;
  protected readonly kind = signal<LibraryKind>('radio-profiles');
  protected readonly entries = signal<Entry[]>([]);
  protected readonly selectedId = signal<string | null>(null);
  protected readonly text = signal('{}');
  protected readonly busy = signal(false);
  protected readonly message = signal<{ ok: boolean; text: string } | null>(null);
  /** Server 422 report: `path` is a JSON pointer into the body sent */
  protected readonly serverIssues = signal<{ path: string; message: string }[]>([]);
  protected readonly conflict = signal(false);

  protected readonly schemaFile = computed(() => LIBRARY_KINDS.find((k) => k.kind === this.kind())?.schemaFile ?? '');
  private readonly parsed = computed<{ value?: unknown; error?: string }>(() => {
    try {
      return { value: JSON.parse(this.text()) };
    } catch (e) {
      return { error: (e as Error).message };
    }
  });
  protected readonly parseError = computed(() => this.parsed().error ?? null);
  protected readonly validation = computed(() => {
    const p = this.parsed();
    if (p.error) return { ok: false, issues: [] };
    return this.validator.validateLibrary(this.kind(), p.value);
  });
  protected readonly draftName = computed(() => {
    const v = this.parsed().value as Entry | undefined;
    return (typeof v?.name === 'string' && v.name) || (this.selectedId() ? this.selectedId()! : 'New entry');
  });
  protected readonly isAdmin = computed(() => !!this.creds.adminTokenSignal());
  protected readonly draftVerified = computed(() => (this.parsed().value as Entry | undefined)?.verified === true);
  protected readonly draftSource = computed(() => this.sourceText((this.parsed().value as Entry | undefined) ?? {}));
  protected readonly draftVersion = computed(() => (this.parsed().value as Entry | undefined)?.version);

  async ngOnInit(): Promise<void> {
    await this.reload();
  }

  protected async selectKind(k: LibraryKind): Promise<void> {
    this.kind.set(k);
    await this.reload();
  }

  private async reload(): Promise<void> {
    this.message.set(null);
    try {
      const list = await this.api.library<Entry>(this.kind());
      this.entries.set(list);
      if (list[0]) this.select(list[0]);
      else this.newEntry();
    } catch (e) {
      this.message.set({ ok: false, text: ApiError.from(e).message });
    }
  }

  protected select(e: Entry): void {
    this.selectedId.set(e.id ?? null);
    this.text.set(JSON.stringify(e, null, 2));
    this.serverIssues.set([]);
    this.message.set(null);
    this.conflict.set(false);
  }

  /** After a 409: fetch the item as it is now (its version) and show it. */
  protected async loadCurrent(): Promise<void> {
    const id = this.selectedId();
    if (!id) return;
    try {
      const cur = await this.api.libraryItem<Entry>(this.kind(), id);
      this.select(cur);
      this.message.set({ ok: true, text: `Loaded version ${cur.version ?? '?'}. Make your change again and save.` });
    } catch (e) {
      this.message.set({ ok: false, text: ApiError.from(e).message });
    }
  }

  protected newEntry(): void {
    this.selectedId.set(null);
    this.text.set(JSON.stringify({ id: '', name: '', version: 1, verified: false }, null, 2));
    this.serverIssues.set([]);
  }

  /** The entry's source, or the sensitivity source of a radio profile. */
  protected sourceText(e: Entry): string {
    const s = (e['source'] ?? (e['sensitivity'] as Entry | undefined)?.['source']) as { kind?: string; reference?: string; url?: string } | undefined;
    if (!s?.reference) return '';
    return `${s.kind ?? 'source'}: ${s.reference}`;
  }

  protected summary(e: Entry): string {
    const f = e['frequencyHz'];
    if (typeof f === 'number') return `${(f / 1e6).toFixed(0)} MHz`;
    if (typeof e['gainDbi'] === 'number') return `${e['gainDbi']} dBi`;
    if (typeof e['capacityAh'] === 'number') return `${Math.round((e['capacityAh'] as number) * 1000)} mAh · ${e['cellsSeries']}S ${e['chemistry']}`;
    if (typeof e['massKg'] === 'number') return `${e['massKg']} kg · cruise ${e['cruiseAirspeedMps']} m/s`;
    return String(e['kind'] ?? e.id ?? '');
  }

  protected async save(): Promise<void> {
    const value = this.parsed().value as Entry;
    if (!this.validation().ok || !value) return;
    this.busy.set(true);
    this.serverIssues.set([]);
    this.message.set(null);
    this.conflict.set(false);
    try {
      const id = this.selectedId();
      // PUT sends the version that was edited (it is in the draft); the server answers with the new one
      const saved = (id ? await this.api.saveLibraryEntry(this.kind(), id, value) : await this.api.addLibraryEntry(this.kind(), value)) as Entry | null;
      const list = await this.api.library<Entry>(this.kind());
      this.entries.set(list);
      const current = saved && typeof saved === 'object' ? saved : list.find((e) => e.id === (value.id ?? id));
      if (current) this.select(current);
      else this.selectedId.set(value.id ?? id);
      this.message.set({ ok: true, text: `Saved${current?.version !== undefined ? ` as version ${current.version}` : ''}.` });
      void this.data.reloadLibrary();
    } catch (e) {
      const err = ApiError.from(e);
      if (err.status === 401 || err.status === 403) this.message.set({ ok: false, text: 'Set the admin token first (top right).' });
      else if (err.status === 422) {
        this.serverIssues.set(err.issues);
        this.message.set({ ok: false, text: 'The server rejected the entry.' });
      } else if (err.status === 409) {
        this.conflict.set(!!this.selectedId());
        this.message.set({ ok: false, text: this.selectedId() ? `Someone saved this item after version ${value.version ?? '?'} that you edited. Nothing was saved. ${err.message}` : err.message });
      } else this.message.set({ ok: false, text: err.message });
    } finally {
      this.busy.set(false);
    }
  }

  protected async importFile(ev: Event): Promise<void> {
    const f = (ev.target as HTMLInputElement).files?.[0];
    if (!f) return;
    const txt = await f.text();
    this.selectedId.set(null);
    try {
      const v = JSON.parse(txt);
      this.text.set(JSON.stringify(Array.isArray(v) ? v[0] : v, null, 2));
    } catch {
      this.text.set(txt);
    }
  }

  protected exportAll(): void {
    MissionExporter.download({ filename: `library-${this.kind()}.json`, mime: 'application/json', content: JSON.stringify(this.entries(), null, 2) });
  }
}
