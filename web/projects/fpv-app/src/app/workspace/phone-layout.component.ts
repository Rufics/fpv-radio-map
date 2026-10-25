import { Component, DestroyRef, ElementRef, computed, effect, inject, signal, viewChild } from '@angular/core';
import { PillComponent, RfFormat } from '@rfmap/ui-kit';
import { FlightReplay } from '../core/flight-replay.service';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { FlightImportComponent } from '../replay/flight-import.component';
import { ReplaySheetComponent } from '../replay/replay-sheet.component';
import { exampleMissionFor } from '../core/example-mission';
import { WhyDrawerComponent } from '../panels/why-drawer.component';
import { PreviewPanelComponent } from '../panels/preview-panel.component';
import { WeatherBriefingComponent } from '../panels/weather-briefing.component';
import { WeatherCardComponent } from '../panels/weather-card.component';
import { AltitudeView } from '../core/fpv-altitude';
import { AltitudeDrawerComponent } from '../panels/altitude-drawer.component';
import { ClimbAssistantComponent } from '../panels/climb-assistant.component';
import { MapStageComponent } from './map-stage.component';
import { PhoneAltitudeComponent } from './phone-altitude.component';

/** A drag on the sheet's handle further than this, px, pulls it up or down; less is a tap. */
const DRAG_PX = 24;

/**
 * Layout C (phone, view-oriented): a full-screen map under a bottom sheet with the verdict, and the
 * altitude check in big numbers. Pulling the sheet up shows the altitude profile; "why" and the
 * climb assistant open full screen.
 */
@Component({
  selector: 'fpv-phone-layout',
  imports: [PillComponent, MapStageComponent, PhoneAltitudeComponent, AltitudeDrawerComponent, ClimbAssistantComponent, WhyDrawerComponent, PreviewPanelComponent, WeatherCardComponent, WeatherBriefingComponent, ReplaySheetComponent, FlightImportComponent],
  host: { class: 'phone' },
  template: `
    @if (ws.briefingOpen() && ws.mission()) {
      <fpv-weather-briefing />
    } @else if (assistantOpen() && alt.climb()) {
      <section class="why" aria-labelledby="phone-climb-title">
        <div class="why-head">
          <button type="button" class="back" aria-label="Back to mission" (click)="assistantOpen.set(false)">‹</button>
          <b id="phone-climb-title">Climb options</b>
        </div>
        <fpv-climb-assistant />
      </section>
    } @else if (ws.why() && ws.whyLeg(); as leg) {
      <section class="why" aria-labelledby="phone-why-title">
        <div class="why-head">
          <button type="button" class="back" aria-label="Back to mission" (click)="ws.why.set(null)">‹</button>
          <b id="phone-why-title">{{ whyTitle() }}</b>
        </div>
        @if (verdictText(); as t) {
          <div class="reason">{{ t }}</div>
        }
        <fpv-why-drawer [compact]="true" />
        @if (!ws.store.canEdit()) {
          <div class="foot">View-only link. Ask the mission owner to change the route.</div>
        }
      </section>
    } @else {
      <div class="map" [class.over]="ws.layersOpen()">
        <fpv-map-stage class="stage" [fitPadding]="padding()" [compact]="true" [interactiveEdit]="false" [style.--fpv-cover-bottom]="cover() + 'px'" />
        <div class="chips">
          <span class="chip name">{{ ws.mission()?.name ?? 'FPV Radio Map' }}</span>
          <span class="chip-end">
            @if (alt.corridor()) {
              <button
                type="button"
                class="chip view"
                [attr.aria-pressed]="alt.mode() === '3d'"
                [attr.aria-label]="alt.mode() === '3d' ? 'Back to the flat map' : 'Show in 3D'"
                (click)="alt.setMode(alt.mode() === '3d' ? '2d' : '3d')"
              >
                {{ alt.mode() === '3d' ? '2D' : '3D' }}
              </button>
            }
            @if (ws.mission()) {
              <span class="chip role">{{ ws.store.canEdit() ? 'Can edit' : 'View only' }}</span>
            }
          </span>
        </div>
      </div>
      <section class="sheet" [class.up]="sheetUp()" aria-label="Mission summary" #sheet>
        @if (alt.corridor() && planShown()) {
          <button
            type="button"
            class="handle-btn"
            [attr.aria-expanded]="sheetUp()"
            [attr.aria-label]="sheetUp() ? 'Collapse to the summary' : 'Pull up the altitude profile'"
            (click)="tapHandle()"
            (pointerdown)="dragFrom($event)"
            (pointerup)="dragTo($event)"
          >
            <span class="handle"></span>
          </button>
        } @else {
          <div class="handle" aria-hidden="true"></div>
        }
        @if (!ws.mission()) {
          <h2>Plan on a laptop, check here</h2>
          <div class="rf-body">Open a shared mission link, or look at the example route.</div>
          <button type="button" class="rf-btn rf-btn--primary" (click)="example()" [disabled]="!ws.data.loaded()">Use example route</button>
        } @else if (ws.store.preview()) {
          <fpv-preview-panel />
        } @else if (replay.mode() !== 'plan') {
          <fpv-replay-sheet />
        } @else {
          @if (ws.pills().length) {
            <div class="pills">
              @for (p of ws.pills(); track p.key) {
                <rf-pill [tone]="p.tone">{{ p.text }}</rf-pill>
              }
            </div>
          }
          <fpv-phone-altitude (profile)="sheetUp.set(true)" (assistant)="assistantOpen.set(true)" />
          @if (sheetUp() && alt.corridor()) {
            <fpv-altitude-drawer [compact]="true" />
          }
          <div class="rf-eyebrow">Route · {{ ws.routeLengthText() }}{{ ws.store.result() ? ' · tap a leg' : '' }}</div>
          <div class="legs">
            @for (r of ws.legRows(); track r.index) {
              @if (ws.store.result() && r.mark) {
                <button type="button" class="leg" [class.bad]="r.tone === 'blocked'" (click)="ws.openWhy(r.index)" [attr.aria-label]="r.name + ', ' + r.info + ', ' + r.markLabel + '. Show why'">
                  <span>{{ r.name }} · {{ km(r.lengthM) }}</span><span class="mark" [class]="'mark tone-' + r.tone">{{ r.mark }} ›</span>
                </button>
              } @else {
                <div class="leg"><span>{{ r.name }} · {{ km(r.lengthM) }}</span><span class="mark">{{ r.mark }}</span></div>
              }
            }
          </div>
          @if (ws.weatherMode() && ws.mission()!.waypoints.length) {
            <fpv-weather-card [routeColours]="true" />
          }
          @if (!ws.store.result() && ws.store.canEdit()) {
            <button type="button" class="rf-btn rf-btn--primary" (click)="ws.store.calculate()" [disabled]="busy()">{{ busy() ? 'Calculating…' : 'Calculate' }}</button>
          }
          @if (ws.batteryFailure()) {
            <div class="note bad">Battery check did not finish; the battery verdict is missing, not assumed fine.</div>
          }
          <button type="button" class="rf-btn rf-btn--secondary" (click)="replay.setMode('replay')">Replay a flight</button>
          <div class="foot">{{ ws.store.canEdit() ? 'Editing the route works best on a laptop.' : 'View-only link. Ask the mission owner to change the route.' }}</div>
        }
      </section>
    }
    @if (replay.importOpen()) {
      <fpv-flight-import />
    }
  `,
  styles: `
    :host { position: relative; display: block; height: 100%; overflow: hidden; background: var(--rf-bg); }
    .map { position: absolute; inset: 0; }
    /* the layer panel opens over the sheet */
    .map.over { z-index: 7; }
    .stage { position: absolute; inset: 0; }
    .chips { position: absolute; top: 12px; left: 12px; right: 12px; display: flex; justify-content: space-between; align-items: center; z-index: 5; pointer-events: none; }
    .chip { padding: 6px 10px; border-radius: 10px; background: rgba(16, 22, 29, 0.94); font-weight: 600; }
    .chip-end { display: inline-flex; gap: 8px; align-items: center; }
    .chip.view { pointer-events: auto; min-width: 44px; height: 36px; border: 1px solid var(--rf-border); color: var(--rf-text); font: inherit; font-weight: 600; }
    .chip.view[aria-pressed='true'] { background: var(--rf-surface-3); border-color: var(--rf-text-2); }
    .chip.role { padding: 4px 8px; border-radius: 6px; color: var(--rf-text-2); font-size: 12px; font-weight: 400; }
    .sheet { position: absolute; left: 0; right: 0; bottom: 0; top: calc(56% - 30px); z-index: 6; transition: top 0.2s ease; padding: 10px 16px calc(20px + env(safe-area-inset-bottom)); border-radius: 16px 16px 0 0; background: var(--rf-surface-2); border-top: 1px solid var(--rf-border); display: flex; flex-direction: column; gap: 12px; overflow-y: auto; }
    .sheet.up { top: 12%; }
    .handle-btn { align-self: center; flex: none; width: 88px; height: 24px; margin: -6px 0 -4px; padding: 0; border: 0; background: none; display: grid; place-items: center; touch-action: none; }
    .handle { width: 40px; height: 4px; border-radius: 2px; background: var(--rf-border-strong); align-self: center; flex: none; }
    h2 { margin: 0; font-size: 20px; }
    .pills { display: flex; flex-wrap: wrap; gap: 6px; }
    .legs { display: flex; flex-direction: column; gap: 6px; }
    .leg { height: 44px; padding: 0 12px; border: 0; border-radius: 10px; background: var(--rf-surface-3); color: var(--rf-text); display: flex; justify-content: space-between; align-items: center; font-size: 14px; flex: none; }
    .leg.bad { border: 1px solid var(--rf-blocked-border); background: var(--rf-blocked-bg); }
    .mark { color: var(--rf-ok-text); font-weight: 600; }
    .mark.tone-blocked { color: var(--rf-blocked-text); }
    .mark.tone-warn { color: var(--rf-warn-text); }
    .foot { margin-top: auto; font-size: 12px; color: var(--rf-text-2); text-align: center; }
    .note.bad { padding: 10px; border-radius: 10px; background: var(--rf-blocked-bg); color: var(--rf-blocked-text); font-size: 13px; }
    .why { position: absolute; inset: 0; padding: 16px; display: flex; flex-direction: column; gap: 12px; overflow-y: auto; }
    .why-head { display: flex; align-items: center; gap: 8px; font-size: 18px; }
    .back { width: 44px; height: 44px; border: 0; border-radius: 10px; background: var(--rf-surface-3); color: var(--rf-text); font-size: 18px; flex: none; }
    .reason { padding: 12px; border-radius: 12px; background: var(--rf-blocked-bg); border: 1px solid var(--rf-blocked-border); color: var(--rf-text-3); line-height: 1.45; }
  `,
})
export class PhoneLayoutComponent {
  protected readonly ws = inject(FpvWorkspace);
  protected readonly replay = inject(FlightReplay);
  protected readonly alt = inject(AltitudeView);
  /** The sheet pulled up to the altitude profile. */
  protected readonly sheetUp = signal(false);
  /** The climb assistant's strategies, full screen. */
  protected readonly assistantOpen = signal(false);
  /** How much of the map the sheet covers, px: the route is fitted and the chase timeline sits above it. */
  protected readonly cover = signal(0);
  /** Fits the route above the sheet, but never into less than the top half of the screen (the sheet pulled up covers more). */
  protected readonly padding = computed(() => ({ top: 70, bottom: Math.min(this.cover() + 40, Math.round(this.height() / 2)), left: 70, right: 90 }));
  private readonly host = inject(ElementRef<HTMLElement>);
  private readonly height = signal(0);
  /** The plan's own sheet, not a preview or a replay over it. */
  protected readonly planShown = computed(() => !!this.ws.mission() && !this.ws.store.preview() && this.replay.mode() === 'plan');
  private readonly sheet = viewChild('sheet', { read: ElementRef });
  private dragY: number | null = null;
  private dragged = false;
  protected readonly busy = computed(() => ['saving', 'running'].includes(this.ws.store.calcState()));

  constructor() {
    const ro = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver((e) => {
            this.cover.set((e[0]?.target as HTMLElement | undefined)?.offsetHeight ?? 0);
            this.height.set((this.host.nativeElement as HTMLElement).offsetHeight);
          });
    effect((onCleanup) => {
      const el = this.sheet()?.nativeElement as HTMLElement | undefined;
      if (!el) {
        this.cover.set(0);
        return;
      }
      this.cover.set(el.offsetHeight);
      this.height.set((this.host.nativeElement as HTMLElement).offsetHeight);
      ro?.observe(el);
      onCleanup(() => ro?.unobserve(el));
    });
    inject(DestroyRef).onDestroy(() => ro?.disconnect());
    // a fix previewed from the assistant shows on the map, with Apply on the sheet
    effect(() => {
      if (this.ws.store.preview()) {
        this.assistantOpen.set(false);
        this.sheetUp.set(false);
      }
    });
    // without a corridor there is no profile to pull up
    effect(() => {
      if (!this.alt.corridor()) this.sheetUp.set(false);
    });
  }

  protected dragFrom(e: PointerEvent): void {
    this.dragY = e.clientY;
    this.dragged = false;
  }

  /** Up or down by the drag's direction; a short one is left to the tap. */
  protected dragTo(e: PointerEvent): void {
    if (this.dragY === null) return;
    const dy = e.clientY - this.dragY;
    this.dragY = null;
    if (Math.abs(dy) < DRAG_PX) return;
    this.dragged = true;
    this.sheetUp.set(dy < 0);
  }

  protected tapHandle(): void {
    if (this.dragged) {
      this.dragged = false;
      return;
    }
    this.sheetUp.update((u) => !u);
  }

  protected km(m: number): string {
    return RfFormat.format(m, 'km1');
  }

  protected readonly whyTitle = computed(() => {
    const link = this.ws.whyLink();
    const n = this.ws.why()?.leg;
    return link?.reachable === false ? `Why leg ${n} fails` : `Leg ${n}`;
  });

  protected readonly verdictText = computed(() => {
    const v = this.ws.verdict();
    const w = this.ws.why();
    if (!v || !w || v.legIndex !== w.leg) return null;
    const otherOk = (['control', 'video'] as const).filter((k) => k !== v.link).some((k) => this.ws.whyLeg()?.worst?.[k]?.reachable);
    const other = v.link === 'video' ? 'Control' : 'Video';
    const otherText = otherOk ? ` ${other} still works.` : '';
    return `${v.text}${otherText}`;
  });

  protected example(): void {
    this.ws.store.newMission(exampleMissionFor(this.ws.data));
  }
}
