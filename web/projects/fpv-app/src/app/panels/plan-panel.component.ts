import { Component, computed, inject } from '@angular/core';
import {
  AltitudeRef,
  EvaluateMode,
  FlightMode,
  JobPart,
  JobProgressComponent,
  LinkKind,
  Mission,
  MissionAltitude,
  RadioProfile,
  RfFormat,
  RfNumComponent,
  SegmentedComponent,
  SlotHostComponent,
  StepperComponent,
} from '@rfmap/ui-kit';
import { FpvWorkspace } from '../core/fpv-workspace.service';
import { exampleMissionFor } from '../core/example-mission';
import { MissionTimeComponent } from './mission-time.component';
import { PreviewPanelComponent } from './preview-panel.component';
import { StateCardsComponent } from './state-cards.component';
import { FlightChecksComponent } from './flight-checks.component';
import { WeatherCardComponent } from './weather-card.component';
import { ComparePanelComponent } from './compare-panel.component';
import { CompareService } from '../core/fpv-core';

/** Side panel of layout A: new mission, links, radios, time, route, calculate, results. */
@Component({
  selector: 'fpv-plan-panel',
  imports: [SegmentedComponent, JobProgressComponent, SlotHostComponent, StepperComponent, MissionTimeComponent, PreviewPanelComponent, StateCardsComponent, FlightChecksComponent, RfNumComponent, ComparePanelComponent, WeatherCardComponent],
  template: `
    @if (!ws.mission()) {
      @if (ws.placing() === 'pilot') {
        <div class="rf-eyebrow">New mission · step 1 of 3</div>
        <h2>Place the pilot</h2>
        <div class="rf-body">Click the map where you will stand. Then add route points; the last point is the target.</div>
        <button type="button" class="rf-btn rf-btn--secondary" (click)="ws.placing.set(null)">Cancel</button>
      } @else {
        <div class="rf-eyebrow">New mission</div>
        <h2>Plan your first flight</h2>
        <ol class="steps">
          <li><b>Place the pilot</b> on the map.</li>
          <li><b>Place a target</b> or draw a route.</li>
          <li><b>Pick your radios</b> from the library.</li>
        </ol>
        <button type="button" class="rf-btn rf-btn--primary rf-btn--lg" (click)="useExample()" [disabled]="!ws.data.loaded()">Use example route</button>
        <button type="button" class="rf-btn rf-btn--secondary" (click)="startPlacing()" [disabled]="!ws.data.loaded()">Place pilot on the map</button>
      }
    } @else if (ws.store.preview() && ws.store.result()) {
      <fpv-preview-panel />
    } @else {
      @let m = ws.mission()!;
      @let canEdit = ws.store.canEdit();
      @if (!canEdit) {
        <div class="viewonly" role="note">View only · results as the owner saved them</div>
      }
      <div class="rf-eyebrow">Flight</div>
      @if (canEdit) {
        <rf-segmented label="Flight" [options]="flights" [value]="m.flightMode ?? 'manual'" (valueChange)="setFlight($event)" />
      }
      <div class="rf-caption">{{ flightHint(m) }}</div>
      <label class="drone-row">
        <span class="rf-eyebrow">Drone</span>
        <select class="rf-input" [disabled]="!canEdit" (change)="setDrone($any($event.target).value)" aria-label="Drone">
          @for (d of ws.data.drones(); track d.id) {
            <option [value]="d.id" [selected]="d.id === m.drone">{{ d.name }} · {{ d.airframe === 'fixed-wing' ? 'fixed-wing' : 'multirotor' }}</option>
          }
        </select>
      </label>
      @if (droneHint(m); as h) {
        <div class="rf-caption">{{ h }}</div>
      }
      <div class="rf-eyebrow">Links to check</div>
      @if (canEdit) {
        <rf-segmented label="Links to check" [options]="modes" [value]="m.evaluate" (valueChange)="setMode($event)" />
      }
      <div class="list">
        @for (l of radios(); track l.kind) {
          <button type="button" class="row radio" (click)="ws.radioSetup.set(l.kind)" [disabled]="!canEdit" [attr.aria-label]="'Radio setup for the ' + l.kind + ' link: ' + l.name">
            <span><b [class]="l.kind">{{ l.letter }}</b> · {{ l.name }}</span><span class="muted">{{ l.source }}</span>
          </button>
        }
      </div>

      <fpv-mission-time [editable]="canEdit" />

      @if (ws.weatherMode() && m.waypoints.length) {
        <fpv-weather-card />
      }

      <div class="section-head">
        <span class="rf-eyebrow">Route · {{ ws.routeLengthText() }}</span>
        @if (canEdit && m.waypoints.length) {
          <button type="button" class="rf-btn rf-btn--dense rf-btn--text" [attr.aria-pressed]="ws.editingRoute()" (click)="toggleEdit()">{{ ws.editingRoute() ? 'Done editing' : 'Edit route' }}</button>
        }
      </div>
      <div class="list">
        <div class="row pilot">
          <span>A · Pilot</span>
          <span class="info rf-mono" [title]="fmt.full(m.pilot.aglM, 'm') + ' above ground'">{{ fmt.format(m.pilot.aglM, 'm') }} AGL</span>
          @if (canEdit) {
            <button type="button" class="rf-btn rf-btn--dense rf-btn--text" (click)="movePilot()" [disabled]="busy() || ws.placing() === 'pilot'" title="Click the map at the new pilot position">Move on map</button>
          }
        </div>
        @if (ws.editingRoute()) {
          <div class="edit">
            <rf-stepper label="Height above ground, pilot" [value]="m.pilot.aglM" unit="m AGL" [step]="1" [max]="1500" [dense]="true" (valueChange)="ws.store.setPilotAgl($event)" />
          </div>
          <div class="edit ref">
            <rf-segmented label="Waypoint heights" [options]="refs" [value]="alt.ref(m)" (valueChange)="setRef($event)" />
          </div>
          <div class="rf-caption">{{ refHint(m) }}</div>
        }
        @for (r of ws.legRows(); track r.index) {
          @if (ws.store.result() && r.mark && !ws.editingRoute()) {
            <button type="button" class="row leg" [class]="'row leg tone-' + r.tone" (click)="ws.openWhy(r.index)" [attr.aria-label]="r.name + ', ' + r.info + (r.markLabel ? ', ' + r.markLabel : '') + '. Show details'">
              <span>{{ r.name }}</span><span class="info rf-mono" [title]="r.infoTitle">{{ r.info }}</span><span class="mark" aria-hidden="true">{{ r.mark }}</span>
            </button>
          } @else {
            <div class="row leg" [class]="'row leg tone-' + r.tone">
              <span>{{ r.name }}</span><span class="info rf-mono" [title]="r.infoTitle">{{ r.info }}</span><span class="mark" [attr.aria-label]="r.markLabel">{{ r.mark }}</span>
            </div>
          }
          @if (ws.editingRoute() && !r.isReturn) {
            <div class="edit">
              <rf-stepper [label]="'Height ' + alt.basis(m) + ', point ' + r.to" [value]="r.heightM" [unit]="alt.unit(m)" [step]="5" [min]="alt.ref(m) === 'launch' ? -1500 : 0" [max]="1500" [dense]="true" (valueChange)="setHeight(r.index - 1, $event)" />
              <button type="button" class="rf-btn rf-btn--dense rf-btn--danger" (click)="ws.store.removeWaypoint(r.index - 1)" [disabled]="m.waypoints.length <= 1" [attr.aria-label]="'Delete point ' + r.to">Delete</button>
            </div>
          }
        }
        @if (m.waypoints.length === 0) {
          <div class="rf-caption">No route yet. Click the map to add points.</div>
        }
        @if (ws.editingRoute() || m.waypoints.length === 0) {
          <button type="button" class="rf-btn rf-btn--sm add" (click)="ws.placing.set('waypoints')" [disabled]="!canEdit">+ Add point at end</button>
        }
      </div>

      <div class="battery-row">
        <span>Battery range</span>
        <span class="rf-mono muted">{{ ws.batteryPlanText() }}</span>
      </div>

      @if (showCalculate()) {
        <button type="button" class="rf-btn rf-btn--primary rf-btn--lg" (click)="calculate()" [disabled]="m.waypoints.length === 0 || busy()">
          {{ ws.store.resultStale() ? 'Recalculate' : 'Calculate' }}
        </button>
        @if (ws.store.resultStale()) {
          <div class="rf-caption">The mission changed since the last calculation; results below are from before.</div>
        }
      }
      @if (canEdit && ws.store.canUndoMove()) {
        <button type="button" class="rf-btn rf-btn--sm rf-btn--secondary" (click)="ws.store.undoMove()">Undo move</button>
      }
      @if (busy()) {
        <rf-job-progress [job]="ws.store.job()" [extraParts]="routePart()" (cancelRequested)="ws.store.cancel()" />
        <div class="rf-caption">Finished parts appear on the map right away.</div>
      }
      @if (ws.store.calcState() === 'cancelled') {
        <div class="rf-caption" role="status">Calculation cancelled. Results that were finished are kept.</div>
      }

      <fpv-state-cards />

      @if (ws.verdict(); as v) {
        <section class="verdict rf-card rf-card--blocked" aria-labelledby="verdict-title">
          <div class="vhead"><b id="verdict-title">{{ v.title }}</b><span class="badge">{{ v.badge }}</span></div>
          <div class="rf-body">{{ v.text }}</div>
          <dl class="rf-breakdown">
            @if (v.diffractionDb !== undefined) {<dt>Diffraction</dt><dd><rf-num [value]="-v.diffractionDb" kind="db" /></dd>}
            <dt>Margin</dt><dd class="is-bad"><rf-num [value]="v.marginDb" kind="db+" /></dd>
          </dl>
          <rf-slot-host name="verdict.reasons" [data]="v" />
          <div class="actions">
            @if (v.fix && canEdit) {
              <button type="button" class="rf-btn rf-btn--primary" (click)="ws.store.previewFix(v.fix)">Preview fix</button>
            }
            <button type="button" class="rf-btn rf-btn--outline-ok" (click)="ws.openWhy(v.legIndex, v.link)">Show why</button>
          </div>
        </section>
      }
      @if (ws.allClearText(); as t) {
        <section class="rf-card rf-card--ok clear">
          <b>{{ ws.allClearTitle() }}</b>
          <div class="rf-body">{{ t }}</div>
        </section>
      }
      <fpv-flight-checks />
      @if (ws.batteryCard(); as b) {
        <section class="rf-card rf-card--warn battery">
          <b>Battery will not last with return</b>
          <div class="rf-body">{{ b.text }}</div>
          <div class="suggest"><b>Suggested:</b> {{ b.suggestion }}</div>
          @if (b.fix && canEdit) {
            <button type="button" class="rf-btn rf-btn--warn" (click)="ws.store.previewFix(b.fix)">Preview turn-back</button>
          }
        </section>
      }

      @if (ws.store.result() || cmp.options()) {
        <fpv-compare-panel />
      }

      <rf-slot-host class="slot" name="side.panels" />

      @if (ws.store.result() || ws.store.calcState() !== 'idle') {
        <button type="button" class="rf-btn rf-btn--muted" (click)="ws.resetToEmpty()">New mission</button>
      }
    }
  `,
  styles: `
    :host { display: flex; flex-direction: column; gap: 14px; }
    h2 { margin: 0; font-size: 22px; }
    .steps { margin: 0; padding-left: 20px; display: flex; flex-direction: column; gap: 10px; color: var(--rf-text-3); line-height: 1.45; }
    .steps b { color: var(--rf-text); }
    .viewonly { padding: 8px 10px; border-radius: 8px; background: var(--rf-surface-3); color: var(--rf-text-2); font-size: 13px; }
    .list { display: flex; flex-direction: column; gap: 6px; font-size: 13px; }
    .row { display: flex; justify-content: space-between; align-items: center; gap: 10px; padding: 8px 10px; min-height: 36px; border-radius: 8px; background: var(--rf-surface-3); border: 0; color: var(--rf-text); text-align: left; font-size: 13px; }
    button.row:hover:not(:disabled) { background: var(--rf-selected); }
    button.row:disabled { color: var(--rf-text); }
    .radio b.control { color: var(--rf-ok-text); }
    .radio b.video { color: var(--rf-video-text); }
    .muted { color: var(--rf-text-2); }
    .leg { display: grid; grid-template-columns: 1fr auto auto; }
    .leg .info { color: var(--rf-text-2); }
    .leg .mark { min-width: 28px; text-align: right; color: var(--rf-ok-text); font-weight: 600; }
    .leg.tone-blocked { background: var(--rf-blocked-chip); }
    .leg.tone-blocked .mark { color: var(--rf-blocked-text); }
    .leg.tone-warn .mark { color: var(--rf-warn-text); }
    .edit { display: flex; gap: 8px; align-items: flex-end; padding: 0 0 6px 10px; }
    .edit rf-stepper { flex: 1; }
    .pilot .info { margin-left: auto; }
    .add { border: 1px dashed var(--rf-border-strong); color: var(--rf-text-2); }
    .section-head { display: flex; justify-content: space-between; align-items: center; min-height: 32px; }
    .drone-row { display: flex; flex-direction: column; gap: 6px; }
    .battery-row { display: flex; justify-content: space-between; align-items: center; gap: 8px; }
    .battery-row .rf-mono { font-size: 13px; }
    .verdict, .clear, .battery { display: flex; flex-direction: column; gap: 8px; padding: 14px; }
    .vhead { display: flex; justify-content: space-between; align-items: center; font-size: 16px; }
    .badge { padding: 3px 8px; border-radius: 999px; background: var(--rf-blocked-chip); color: var(--rf-blocked-text); font-weight: 600; font-size: 12px; }
    .actions { display: flex; gap: 8px; }
    .actions .rf-btn { flex: 1; }
    .clear b { font-size: 16px; color: var(--rf-ok-text); }
    .battery > b { font-size: 16px; color: var(--rf-warn-text); }
    .suggest { padding: 8px 10px; border-radius: 8px; background: var(--rf-surface-2); font-size: 13px; line-height: 1.45; }
    .suggest b { color: var(--rf-warn-text); }
    .slot:empty { display: none; }
  `,
})
export class PlanPanelComponent {
  protected readonly ws = inject(FpvWorkspace);
  protected readonly cmp = inject(CompareService);
  protected readonly alt = MissionAltitude;
  protected readonly refs: { value: AltitudeRef; label: string }[] = [
    { value: 'launch', label: 'Above launch' },
    { value: 'agl', label: 'Above ground' },
  ];
  protected readonly modes: { value: EvaluateMode; label: string }[] = [
    { value: 'both', label: 'Both' },
    { value: 'control', label: 'Control' },
    { value: 'video', label: 'Video' },
    { value: 'none', label: 'None' },
  ];
  protected readonly flights: { value: FlightMode; label: string }[] = [
    { value: 'manual', label: 'Pilot' },
    { value: 'autonomous', label: 'Autonomous' },
  ];

  protected readonly busy = this.ws.store.busy;
  protected readonly showCalculate = computed(() => this.ws.store.canEdit() && !this.busy() && (!this.ws.store.result() || this.ws.store.resultStale()));

  protected readonly routePart = computed<JobPart[]>(() => {
    if (!this.busy()) return [];
    return [{ name: 'Link budgets for every leg', state: this.ws.store.result() ? 'done' : 'running', progress: this.ws.store.result() ? 1 : 0.5 }];
  });

  protected readonly radios = computed(() => {
    const m = this.ws.mission();
    if (!m) return [];
    return this.ws.evaluated().map((kind: LinkKind) => {
      const setup = m.links[kind];
      const p = this.ws.data.radio(setup?.radio);
      return { kind, letter: kind === 'control' ? 'C' : 'V', name: p?.name ?? setup?.radio ?? 'not set', source: this.radioSource(p) };
    });
  });

  protected setMode(mode: EvaluateMode): void {
    this.ws.store.setEvaluate(mode);
    this.ws.why.set(null);
  }

  protected setFlight(mode: FlightMode): void {
    this.ws.store.setFlightMode(mode);
    this.ws.why.set(null);
  }

  /** Picks the drone; a fixed-wing also gets its flight envelope checked (fpv.airframe). */
  protected setDrone(id: string): void {
    const fixedWing = this.ws.data.drones().find((d) => d.id === id)?.airframe === 'fixed-wing';
    this.ws.store.update((m) => {
      const modules = { ...m.modules };
      if (fixedWing) modules['fpv.airframe'] = modules['fpv.airframe'] ?? {};
      else delete modules['fpv.airframe'];
      return { ...m, drone: id, modules };
    });
  }

  protected droneHint(m: Mission): string | null {
    const d = this.ws.data.drones().find((x) => x.id === m.drone);
    const w = d?.fixedWing;
    if (!d || d.airframe !== 'fixed-wing' || !w) return null;
    const checked = m.modules?.['fpv.airframe'] ? 'The route is checked against these limits.' : 'The fixed-wing envelope check is off for this mission; pick the drone again to turn it on.';
    return `Climbs at most ${w.maxClimbAngleDeg}°, descends at most ${w.maxDiveAngleDeg}°, banks ${w.maxBankDeg}°, stalls at ${RfFormat.format(w.stallSpeedMps, 'mps')}. ${checked}`;
  }

  protected flightHint(m: Mission): string {
    if ((m.flightMode ?? 'manual') === 'manual') return 'The pilot flies the route over the control link; losing it hands the drone to the failsafe.';
    return 'The route is uploaded before take-off and the autopilot flies it on its own. The links are optional: where one drops, the pilot cannot take over, but the flight goes on.';
  }

  protected setHeight(index: number, heightM: number): void {
    this.ws.store.update((m) => ({ ...m, waypoints: m.waypoints.map((w, i) => (i === index ? MissionAltitude.withHeight(m, w, heightM) : w)) }));
  }

  protected setRef(ref: AltitudeRef): void {
    this.ws.store.update((m) => MissionAltitude.withRef(m, ref));
  }

  protected refHint(m: Mission): string {
    return MissionAltitude.ref(m) === 'launch'
      ? 'Heights are above the launch point, as a barometric altimeter reads them. The drone climbs or descends evenly between points and does not follow the terrain.'
      : 'Heights are above the ground under the drone. Between points the drone follows the terrain, which costs climbs over every rise.';
  }

  protected toggleEdit(): void {
    this.ws.editingRoute.update((v) => !v);
    if (!this.ws.editingRoute()) this.ws.placing.set(null);
  }

  protected movePilot(): void {
    this.ws.why.set(null);
    this.ws.placing.set('pilot');
  }

  protected useExample(): void {
    this.ws.store.newMission(exampleMissionFor(this.ws.data));
  }

  protected startPlacing(): void {
    // A skeleton mission: radios from the drone's library defaults; pilot and route come from map clicks.
    this.ws.pendingMission.set({ ...exampleMissionFor(this.ws.data), name: 'New mission', waypoints: [] });
    this.ws.placing.set('pilot');
  }

  protected calculate(): void {
    this.ws.editingRoute.set(false);
    this.ws.placing.set(null);
    void this.ws.store.calculate();
  }

  protected readonly fmt = RfFormat;

  private radioSource(p: RadioProfile | undefined): string {
    if (!p) return 'pick one';
    return p.verified ? 'library' : 'library · unverified';
  }
}
