import { Component, ElementRef, computed, inject, input, signal, viewChild } from '@angular/core';
import { Router } from '@angular/router';
import { ApiError, CredentialStore, RfmapApiClient } from '@rfmap/ui-kit';

/** PIN entry for a shared link (/m/v/:token view, /m/e/:token edit). */
@Component({
  selector: 'fpv-pin-entry',
  template: `
    <div class="wrap">
      <div class="rf-eyebrow">Shared mission · {{ role() === 'e' ? 'can edit' : 'view only' }}</div>
      <h1>{{ missionName() ?? 'Shared mission' }}</h1>
      @switch (state()) {
        @case ('revoked') {
          <div class="card bad" role="alert"><b>This link was revoked.</b> The mission owner turned it off. Ask them for a new link.</div>
        }
        @case ('paused') {
          <div class="card warn" role="alert"><b>Too many wrong PINs.</b> This link is paused for 15 minutes{{ retryText() }}. Try again later or ask the owner for the PIN.</div>
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
          <div class="rf-caption fine">After 5 wrong tries the link pauses for 15 minutes.</div>
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
export class PinEntryPage {
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
  protected readonly state = signal<'entry' | 'paused' | 'revoked'>('entry');
  protected readonly retryAfter = signal<number | null>(null);
  /** @gap the contract gives the mission name only after a successful PIN. */
  protected readonly missionName = signal<string | null>(null);

  protected readonly dots = computed(() => Array.from({ length: 6 }, (_, i) => ({ filled: i < this.pin().length, current: i === this.pin().length })));
  protected readonly retryText = computed(() => {
    const s = this.retryAfter();
    if (!s) return '';
    const until = new Date(Date.now() + s * 1000).toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit' });
    return ` (until ${until})`;
  });

  protected focus(): void {
    this.pinInput()?.nativeElement.focus();
  }

  protected onType(v: string): void {
    this.pin.set(v.replace(/\D/g, '').slice(0, 6));
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
      const err = ApiError.from(e);
      this.pin.set('');
      if (err.status === 410) this.state.set('revoked');
      else if (err.status === 429) {
        this.retryAfter.set(err.retryAfterS ?? 900);
        this.state.set('paused');
      } else if (err.status === 401) {
        const left = (err.body as { triesLeft?: number; remainingTries?: number } | undefined)?.triesLeft ?? (err.body as { remainingTries?: number } | undefined)?.remainingTries;
        this.error.set(left !== undefined ? `Wrong PIN. ${left} ${left === 1 ? 'try' : 'tries'} left before the link pauses.` : 'Wrong PIN.');
      } else if (err.status === 404) this.error.set('This link does not exist. Check that you copied all of it.');
      else this.error.set(err.message || 'The server could not be reached.');
    } finally {
      this.busy.set(false);
    }
  }
}
