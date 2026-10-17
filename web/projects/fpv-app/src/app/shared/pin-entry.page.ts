import { Component, ElementRef, OnInit, computed, inject, input, signal, viewChild } from '@angular/core';
import { Router } from '@angular/router';
import { ApiError, CredentialStore, RfmapApiClient } from '@rfmap/ui-kit';

/** PIN entry for a shared link (/m/v/:token view, /m/e/:token edit). */
@Component({
  selector: 'fpv-pin-entry',
  template: `
    <div class="wrap">
      <div class="rf-eyebrow">Shared mission · {{ effectiveRole() === 'edit' ? 'can edit' : 'view only' }}</div>
      <h1>{{ missionName() ?? 'Shared mission' }}</h1>
      @if (expiresText(); as x) {<div class="rf-caption">{{ x }}</div>}
      @switch (state()) {
        @case ('loading') {
          <div class="rf-caption" role="status">Checking the link…</div>
        }
        @case ('gone') {
          <div class="card bad" role="alert"><b>This link no longer works.</b> {{ goneText() }} Ask the mission owner for a new link.</div>
        }
        @case ('unknown') {
          <div class="card bad" role="alert"><b>This link does not exist.</b> Check that you copied all of it.</div>
        }
        @case ('paused') {
          <div class="card warn" role="alert"><b>{{ pausedTitle() }}</b> This link is paused{{ retryText() }}. Try again later or ask the owner for the PIN.</div>
          <button type="button" class="rf-btn rf-btn--secondary" (click)="check()">Try again</button>
        }
        @default {
          <label class="hint" for="pin-input">Enter the 6-digit PIN you were given.</label>
          <input #pinInput id="pin-input" class="real" inputmode="numeric" autocomplete="one-time-code" maxlength="6" [value]="pin()" (input)="onType($any($event.target).value)" (keydown.enter)="open()" aria-describedby="pin-status" />
          <div class="dots" (click)="focus()" aria-hidden="true">
            @for (d of dots(); track $index) {
              <div class="dot" [class.cur]="d.current" [class.err]="!!error()">{{ d.filled ? '•' : '' }}</div>
            }
          </div>
          <div id="pin-status" class="status" role="status" aria-live="polite">{{ error() ?? '' }}</div>
          <div class="keys">
            @for (k of keys; track $index) {
              @if (k === '') {
                <span></span>
              } @else {
                <button type="button" class="key" [attr.aria-label]="k === '⌫' ? 'Delete digit' : 'Digit ' + k" (click)="press(k)" [disabled]="busy()">{{ k }}</button>
              }
            }
          </div>
          <button type="button" class="rf-btn rf-btn--primary open" (click)="open()" [disabled]="pin().length !== 6 || busy()">{{ busy() ? 'Opening…' : 'Open mission' }}</button>
          <div class="rf-caption fine">After 5 wrong tries the link pauses for 15 minutes, longer after each further pause.</div>
        }
      }
    </div>
  `,
  styles: `
    :host { display: block; min-height: 100%; background: var(--rf-bg); }
    .wrap { max-width: 400px; min-height: 100dvh; margin: 0 auto; padding: 56px 24px 32px; display: flex; flex-direction: column; gap: 16px; }
    h1 { margin: 0; font-size: 26px; }
    .hint { color: var(--rf-text-2); }
    .real { position: absolute; opacity: 0; width: 1px; height: 1px; pointer-events: none; }
    .dots { display: grid; grid-template-columns: repeat(6, minmax(0, 1fr)); gap: 8px; cursor: text; }
    .dot { height: 52px; border-radius: 10px; border: 1px solid var(--rf-border); background: var(--rf-surface-2); display: flex; align-items: center; justify-content: center; font-size: 22px; }
    .dot.cur { border-color: var(--rf-ok); }
    .dot.err { border-color: var(--rf-blocked); }
    .status { min-height: 18px; color: var(--rf-blocked-text); font-size: 13px; }
    .keys { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 10px; }
    .key { height: 56px; border: 0; border-radius: 12px; background: var(--rf-surface-3); color: var(--rf-text); font-size: 20px; font-family: var(--rf-font-mono); }
    .key:active { background: var(--rf-selected); }
    .open { margin-top: auto; height: 52px; font-size: 16px; }
    .fine { text-align: center; }
    .card { padding: 14px; border-radius: 12px; line-height: 1.45; color: var(--rf-text-3); }
    .card.bad { background: var(--rf-blocked-bg); border: 1px solid var(--rf-blocked-border); }
    .card.bad b { color: var(--rf-blocked-text); }
    .card.warn { background: var(--rf-warn-bg); border: 1px solid var(--rf-warn-border); }
    .card.warn b { color: var(--rf-warn-text); }
  `,
})
export class PinEntryPage implements OnInit {
  readonly role = input<string>('v');
  readonly token = input.required<string>();
  private readonly api = inject(RfmapApiClient);
  private readonly creds = inject(CredentialStore);
  private readonly router = inject(Router);
  private readonly pinInput = viewChild<ElementRef<HTMLInputElement>>('pinInput');

  protected readonly keys = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '', '0', '⌫'];
  protected readonly pin = signal('');
  protected readonly error = signal<string | null>(null);
  protected readonly busy = signal(false);
  protected readonly state = signal<'loading' | 'entry' | 'paused' | 'gone' | 'unknown'>('loading');
  /** Paused until this time (ms), from LinkInfo.pausedUntil or the 429 retryAfterS. */
  protected readonly pausedUntil = signal<number | null>(null);
  protected readonly pausedByRate = signal(false);
  /** From the public GET /links/{token}, before the PIN. */
  protected readonly missionName = signal<string | null>(null);
  protected readonly infoRole = signal<'edit' | 'view' | null>(null);
  protected readonly expiresAt = signal<string | null>(null);
  protected readonly goneMessage = signal<string | null>(null);

  protected readonly effectiveRole = computed(() => this.infoRole() ?? (this.role() === 'e' ? 'edit' : 'view'));
  protected readonly expiresText = computed(() => {
    const x = this.expiresAt();
    return x ? `Link valid until ${this.when(Date.parse(x))}` : null;
  });
  protected readonly goneText = computed(() => {
    const x = this.expiresAt();
    if (x && Date.parse(x) <= Date.now()) return `It expired ${this.when(Date.parse(x))}.`;
    const m = this.goneMessage();
    if (m && /expire/i.test(m)) return 'It has expired.';
    if (m && /revoke/i.test(m)) return 'The mission owner revoked it.';
    return 'The mission owner revoked it or it expired.';
  });
  protected readonly pausedTitle = computed(() => (this.pausedByRate() ? 'Too many requests.' : 'Too many wrong PINs.'));

  protected readonly dots = computed(() => Array.from({ length: 6 }, (_, i) => ({ filled: i < this.pin().length, current: i === this.pin().length })));
  protected readonly retryText = computed(() => {
    const t = this.pausedUntil();
    if (!t) return '';
    const min = Math.max(1, Math.ceil((t - Date.now()) / 60e3));
    return ` until ${new Date(t).toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit' })} (about ${min} min)`;
  });

  ngOnInit(): void {
    void this.check();
  }

  private when(ms: number): string {
    return new Date(ms).toLocaleString('en-GB', { day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' });
  }

  /** GET /links/{token}: mission name, role, expiry and pause, without using a PIN try. */
  protected async check(): Promise<void> {
    this.state.set('loading');
    try {
      const info = await this.api.linkInfo(this.token());
      this.missionName.set(info.missionName ?? null);
      this.infoRole.set(info.role ?? null);
      this.expiresAt.set(info.expiresAt ?? null);
      const paused = info.pausedUntil ? Date.parse(info.pausedUntil) : Number.NaN;
      if (Number.isFinite(paused) && paused > Date.now()) {
        this.pausedByRate.set(false);
        this.pausedUntil.set(paused);
        this.state.set('paused');
      } else this.state.set('entry');
    } catch (e) {
      this.handleError(ApiError.from(e), true);
    }
  }

  private handleError(err: ApiError, fromInfo: boolean): void {
    if (err.status === 410) {
      this.goneMessage.set(err.message);
      this.state.set('gone');
    } else if (err.status === 404) {
      this.state.set('unknown');
    } else if (err.status === 429) {
      this.pausedByRate.set(fromInfo);
      this.pausedUntil.set(Date.now() + (err.retryAfterS ?? 900) * 1000);
      this.state.set('paused');
    } else if (err.status === 401) {
      this.state.set('entry');
      const left = (err.body as { triesLeft?: number } | undefined)?.triesLeft;
      this.error.set(left !== undefined ? `Wrong PIN. ${left} ${left === 1 ? 'try' : 'tries'} left before the link pauses.` : 'Wrong PIN.');
    } else {
      // the info call is a convenience: if the server cannot answer it, still let the user try the PIN
      this.state.set('entry');
      if (!fromInfo) this.error.set(err.message || 'The server could not be reached.');
    }
  }

  protected focus(): void {
    this.pinInput()?.nativeElement.focus();
  }

  protected onType(v: string): void {
    this.pin.set(v.replaceAll(/\D/g, '').slice(0, 6));
    this.error.set(null);
    const el = this.pinInput()?.nativeElement;
    if (el && el.value !== this.pin()) el.value = this.pin();
  }

  protected press(k: string): void {
    this.error.set(null);
    if (k === '⌫') this.pin.update((p) => p.slice(0, -1));
    else if (this.pin().length < 6) this.pin.update((p) => p + k);
  }

  protected async open(): Promise<void> {
    if (this.pin().length !== 6 || this.busy()) return;
    this.busy.set(true);
    try {
      const res = await this.api.openLink(this.token(), this.pin());
      this.creds.setSession(res.missionId, res.session);
      this.missionName.set(res.missionName);
      await this.router.navigate(['/mission', res.missionId]);
    } catch (e) {
      this.pin.set('');
      this.handleError(ApiError.from(e), false);
    } finally {
      this.busy.set(false);
    }
  }
}
