import { Injectable, computed, inject, signal } from '@angular/core';
import {
  BatteryResult,
  Fix,
  LegResult,
  LinkKind,
  LinkResult,
  Mission,
  MissionPoint,
  MissionStore,
  Reason,
  RfFormat,
  RouteResult,
  Tone,
  distanceM,
  linksEvaluated,
} from '@rfmap/ui-kit';
import { FpvDataService, estimateRangeM } from './fpv-data.service';

export interface PillVm {
  key: string;
  tone: Tone;
  text: string;
  title?: string;
}

export interface LegRowVm {
  index: number;
  name: string;
  from: string;
  to: string;
  info: string;
  infoTitle: string;
  mark: string;
  markLabel: string;
  tone: 'ok' | 'blocked' | 'warn' | 'none';
  failing: LinkKind[];
  isReturn: boolean;
  lengthM: number;
  aglM: number;
}

export interface VerdictVm {
  legIndex: number;
  title: string;
  badge: string;
  text: string;
  fix?: Fix;
  link: LinkKind;
  diffractionDb?: number;
  marginDb?: number;
}

export const LINK_LETTER: Record<LinkKind, string> = { control: 'C', video: 'V' };
const MINUS = RfFormat.MINUS;

export function pointName(m: Mission, i: number): string {
  if (i < 0) return m.pilot.label ?? 'A';
  const w = m.waypoints[i];
  if (w?.label) return w.label;
  return i === m.waypoints.length - 1 ? 'B' : `WP${i + 1}`;
}

/** A mission whose last point is a turn-back point flies back from there. */
export function endsAtTurnBack(m: Mission): boolean {
  return m.waypoints[m.waypoints.length - 1]?.label === 'T';
}

/** FPV view of the mission state: pills, leg rows, verdict, battery, why-drawer selection. */
@Injectable({ providedIn: 'root' })
export class FpvWorkspace {
  readonly store = inject(MissionStore);
  readonly data = inject(FpvDataService);

  /** Map click mode */
  readonly placing = signal<'pilot' | 'waypoints' | null>(null);
  /** Skeleton used when the pilot is placed on an empty map */
  readonly pendingMission = signal<Mission | null>(null);
  readonly editingRoute = signal(false);
  /** "Show why" selection */
  readonly why = signal<{ leg: number; link: LinkKind } | null>(null);
  readonly shareOpen = signal(false);
  readonly layersOpen = signal(false);
  readonly radioSetup = signal<LinkKind | null>(null);
  /** Link shown in the coverage heatmap */
  readonly coverageLink = signal<LinkKind>('control');
  readonly forecastOffsetH = signal(0);

  readonly mission = this.store.mission;
  readonly shown = this.store.shownMission;
  readonly result = computed(() => (this.store.preview() ? this.store.result() : this.store.result()));
  readonly calculated = computed(() => !!this.store.result());
  readonly evaluated = computed(() => linksEvaluated(this.mission()?.evaluate ?? 'both'));

  /** Coverage link actually shown (only evaluated links have a grid). */
  readonly heatLink = computed<LinkKind>(() => {
    const ev = this.evaluated();
    return ev.includes(this.coverageLink()) ? this.coverageLink() : ev[0];
  });

  readonly batteryFailure = computed(() => this.store.result()?.moduleFailures?.find((f) => (f.module ?? '').startsWith('fpv.battery')) ?? null);

  // ---------- reasons ----------
  /** Reasons modules attach at route level. @gap: assumed at RouteResult.modules[id].reasons */
  readonly moduleReasons = computed<Reason[]>(() => {
    const mods = this.store.result()?.modules ?? {};
    const out: Reason[] = [];
    for (const [id, v] of Object.entries(mods)) {
      const reasons = (v as { reasons?: Reason[] } | null)?.reasons;
      if (Array.isArray(reasons)) out.push(...reasons.map((r) => ({ module: id, ...r })));
    }
    return out;
  });

  readonly batteryReason = computed(() => this.moduleReasons().find((r) => r.module === 'fpv.battery') ?? null);

  // ---------- pills ----------
  readonly pills = computed<PillVm[]>(() => {
    const r = this.store.result();
    const m = this.mission();
    if (!r || !m) return [];
    const pills: PillVm[] = [];
    const legs = r.legs ?? [];
    const missing = legs.some((l) => l.status === 'missing-data');
    for (const kind of ['control', 'video'] as LinkKind[]) {
      const letter = LINK_LETTER[kind];
      if (!this.evaluated().includes(kind)) continue;
      const margins = legs.map((l) => l.worst?.[kind]?.marginDb).filter((x): x is number => x !== undefined);
      if (margins.length === 0) {
        pills.push({ key: kind, tone: 'warn', text: `${letter} ? not calculated` });
        continue;
      }
      const worst = Math.min(...margins);
      const ok = worst >= 0;
      pills.push({
        key: kind,
        tone: ok ? (missing ? 'warn' : 'ok') : 'blocked',
        text: `${letter} ${ok ? (missing ? '?' : '✓') : '✕'} ${RfFormat.format(worst, 'db+')}${missing && ok ? ' · part missing' : ''}`,
        title: `Worst ${kind} margin along the route: ${RfFormat.full(worst, 'db+')}`,
      });
    }
    const b = r.battery;
    if (this.batteryFailure()) pills.push({ key: 'battery', tone: 'warn', text: 'Battery ? not checked', title: 'The battery module failed; the verdict is missing, not assumed fine.' });
    else if (b) {
      if ((b.shortfallM ?? 0) > 0) pills.push({ key: 'battery', tone: 'warn', text: `Battery ! ${RfFormat.format(b.shortfallM, 'km1')} short`, title: RfFormat.full(b.shortfallM, 'km3') + ' short' });
      else if (endsAtTurnBack(m)) pills.push({ key: 'battery', tone: 'ok', text: 'Battery ✓ turn back at T' });
      else pills.push({ key: 'battery', tone: 'ok', text: `Battery ✓ ${RfFormat.format(b.rangeM, 'km1')} range` });
    }
    return pills;
  });

  // ---------- legs ----------
  private legResult(r: RouteResult | null, index: number): LegResult | undefined {
    return r?.legs?.find((l) => l.index === index) ?? r?.legs?.[index - 1];
  }

  /** Legs of the shown mission (preview mission while previewing). */
  readonly legRows = computed<LegRowVm[]>(() => {
    const m = this.shown();
    if (!m) return [];
    const previewing = !!this.store.preview();
    const r = previewing || this.store.resultStale() ? null : this.store.result();
    const pts: MissionPoint[] = [m.pilot, ...m.waypoints];
    const rows: LegRowVm[] = m.waypoints.map((w, i) => {
      const index = i + 1;
      const leg = this.legResult(r, index);
      const len = leg?.lengthM ?? distanceM(pts[i], pts[i + 1]);
      const failing = (['control', 'video'] as LinkKind[]).filter((k) => leg?.worst?.[k] && leg.worst[k]!.reachable === false);
      let mark = '';
      let markLabel = '';
      let tone: LegRowVm['tone'] = 'none';
      if (leg) {
        if (leg.status === 'missing-data') [mark, markLabel, tone] = ['? no data', 'No elevation data, not calculated', 'warn'];
        else if (failing.length) [mark, markLabel, tone] = [`${failing.map((k) => LINK_LETTER[k]).join(' ')} ✕`, `${failing.join(' and ')} link blocked`, 'blocked'];
        else if (leg.status === 'warn') [mark, markLabel, tone] = ['!', 'Warning', 'warn'];
        else [mark, markLabel, tone] = ['✓', 'Reachable', 'ok'];
      }
      const from = pointName(m, i - 1);
      const to = pointName(m, i);
      return {
        index,
        name: `${index} · ${from} → ${to}`,
        from,
        to,
        info: `${RfFormat.format(len, 'km1')} · ${RfFormat.format(w.aglM, 'm')}`,
        infoTitle: `${RfFormat.full(len, 'km3')}, ${RfFormat.full(w.aglM, 'm')} above ground`,
        mark,
        markLabel,
        tone,
        failing,
        isReturn: false,
        lengthM: len,
        aglM: w.aglM,
      };
    });
    if (endsAtTurnBack(m) && m.returnToPilot !== false) {
      const t = m.waypoints[m.waypoints.length - 1];
      const back = this.store.result()?.battery?.turnBack?.returnM ?? distanceM(t, m.pilot);
      rows.push({
        index: rows.length + 1,
        name: `${rows.length + 1} · T → A, return`,
        from: 'T',
        to: 'A',
        info: `${RfFormat.format(back, 'km1')} · ${RfFormat.format(t.aglM, 'm')}`,
        infoTitle: RfFormat.full(back, 'km3'),
        mark: r ? '✓' : '',
        markLabel: r ? 'Return inside the flown area' : '',
        tone: r ? 'ok' : 'none',
        failing: [],
        isReturn: true,
        lengthM: back,
        aglM: t.aglM,
      });
    }
    return rows;
  });

  readonly routeLengthText = computed(() => {
    const m = this.shown();
    const rows = this.legRows();
    if (!m) return '';
    const out = rows.filter((r) => !r.isReturn).reduce((s, r) => s + r.lengthM, 0);
    const ret = rows.find((r) => r.isReturn);
    if (ret) return `${RfFormat.format(out, 'km1')} + ${RfFormat.format(ret.lengthM, 'km1')} back`;
    return RfFormat.format(out, 'km1');
  });

  // ---------- verdict ----------
  readonly verdict = computed<VerdictVm | null>(() => {
    const r = this.store.result();
    const m = this.mission();
    if (!r || !m) return null;
    for (const leg of r.legs ?? []) {
      const failing = (['video', 'control'] as LinkKind[]).filter((k) => leg.worst?.[k]?.reachable === false);
      if (leg.status !== 'blocked' && failing.length === 0) continue;
      const link = failing[0] ?? 'video';
      const reason = (leg.reasons ?? []).find((x) => x.severity === 'blocked') ?? leg.reasons?.[0];
      const lr = leg.worst?.[link];
      const idx = leg.index ?? 0;
      return {
        legIndex: idx,
        title: `Leg ${idx} · ${leg.from ?? pointName(m, idx - 2)} → ${leg.to ?? pointName(m, idx - 1)}`,
        badge: `${failing.map((k) => LINK_LETTER[k]).join(' ')} ✕`,
        text: reason?.text ?? `The ${link} link is ${RfFormat.format(-(lr?.marginDb ?? 0), 'db')} short of the required level.`,
        fix: reason?.fix,
        link,
        diffractionDb: lr?.diffraction?.lossDb,
        marginDb: lr?.marginDb,
      };
    }
    return null;
  });

  readonly missingLeg = computed(() => this.store.result()?.legs?.find((l) => l.status === 'missing-data') ?? null);

  readonly allClearText = computed(() => {
    const r = this.store.result();
    const m = this.mission();
    if (!r || !m || this.verdict() || this.missingLeg()) return null;
    const info = (r.legs ?? []).flatMap((l) => l.reasons ?? []).find((x) => x.severity === 'info' && x.text);
    if (info?.text) return info.text;
    const ev = this.evaluated();
    if (ev.length === 1) {
      const other = ev[0] === 'control' ? 'Video' : 'Control';
      return `${other} is not checked in this mode; the ${ev[0]} link reaches every leg.`;
    }
    return 'Control and video reach every leg of the route.';
  });

  // ---------- battery ----------
  readonly batteryPlanText = computed(() => {
    const m = this.mission();
    if (!m) return '';
    const b = this.data.battery(m.battery);
    const d = this.data.drone(m.drone);
    const calc = this.store.result()?.battery?.rangeM;
    const range = calc ?? estimateRangeM(b, d);
    const label = b ? `${b.cellsSeries}S ${Math.round(b.capacityAh * 1000)} mAh` : (m.battery ?? 'no battery');
    return `${label} · ${range !== null && range !== undefined ? (calc ? '' : '≈ ') + RfFormat.format(range, 'km1') : '—'}`;
  });

  readonly batteryCard = computed(() => {
    const r = this.store.result();
    const m = this.mission();
    const b: BatteryResult | undefined = r?.battery;
    if (!r || !m || !b || (b.shortfallM ?? 0) <= 0) return null;
    const reason = this.batteryReason();
    const tb = b.turnBack;
    const legs = (r.legs ?? []).map((l) => l.lengthM ?? 0);
    let where = '';
    if (tb?.distanceAlongRouteM !== undefined) {
      let acc = 0;
      for (let i = 0; i < legs.length; i++) {
        if (tb.distanceAlongRouteM <= acc + legs[i]) {
          where = ` (${RfFormat.format(tb.distanceAlongRouteM - acc, 'km1')} into leg ${i + 1})`;
          break;
        }
        acc += legs[i];
      }
    }
    return {
      text: reason?.text ?? `The route with the return needs ${RfFormat.format(b.neededM, 'km1')}; the pack gives ${RfFormat.format(b.rangeM, 'km1')}.`,
      suggestion: tb?.distanceAlongRouteM !== undefined ? `turn back at T, ${RfFormat.format(tb.distanceAlongRouteM, 'km1')} along the route${where}, or pick a larger pack in the library.` : 'pick a larger pack in the library.',
      fix: reason?.fix,
    };
  });

  // ---------- header ----------
  readonly timeText = computed(() => {
    const m = this.mission();
    if (!m) return '';
    const planned = m.time.mode === 'planned' && m.time.utc;
    const t = planned ? new Date(m.time.utc!) : new Date();
    const hm = t.toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit' });
    const tier = this.store.result()?.weather?.tier ?? this.data.tierFor(planned ? m.time.utc! : null);
    const tierText = tier === 'hourly' ? 'forecast hourly' : tier === 'long-range' ? 'long-range forecast' : 'no forecast · standard atmosphere';
    const day = planned ? t.toLocaleDateString('en-GB', { weekday: 'short', day: 'numeric', month: 'short' }) + ' · ' : 'Now · ';
    return `${day}${hm} local · ${tierText}`;
  });

  // ---------- why drawer ----------
  readonly whyLeg = computed(() => {
    const w = this.why();
    const r = this.store.result();
    if (!w || !r) return null;
    return this.legResult(r, w.leg) ?? null;
  });

  readonly whyLink = computed<LinkResult | null>(() => {
    const w = this.why();
    return (w && this.whyLeg()?.worst?.[w.link]) || null;
  });

  openWhy(leg: number, link?: LinkKind): void {
    const lr = this.legResult(this.store.result(), leg);
    const failing = (['video', 'control'] as LinkKind[]).find((k) => lr?.worst?.[k]?.reachable === false);
    const any = (['video', 'control'] as LinkKind[]).find((k) => lr?.worst?.[k]);
    const chosen = link ?? failing ?? any;
    if (!chosen) return;
    this.why.set({ leg, link: chosen });
  }

  /** Ground heights of the reference profile, for marker captions ("Pilot · 544 + 2 m"). */
  readonly groundAt = computed(() => {
    const r = this.store.result();
    const legs = r?.legs ?? [];
    const out: { pilot?: number; target?: number } = {};
    for (const leg of legs) {
      const lr = leg.worst?.video ?? leg.worst?.control;
      const pts = lr?.profile?.points;
      if (!pts?.length) continue;
      out.pilot ??= pts[0].groundMslM;
      if (leg === legs[legs.length - 1]) out.target = pts[pts.length - 1].groundMslM;
    }
    return out;
  });

  resetToEmpty(): void {
    this.why.set(null);
    this.placing.set(null);
    this.editingRoute.set(false);
    this.store.clear();
  }
}

export { MINUS };
