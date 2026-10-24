import { Injectable, computed, inject, signal } from '@angular/core';
import {
  BatteryResult,
  Fix,
  LegResult,
  LinkKind,
  LinkResult,
  Mission,
  MissionAltitude,
  MissionPoint,
  MissionStore,
  Reason,
  RfFormat,
  RouteResult,
  Tone,
  WeatherMetric,
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
  /** Height of the leg's end point in the mission's altitude reference. */
  heightM: number;
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
  return m.waypoints.at(-1)?.label === 'T';
}

function linkPillTone(ok: boolean, missing: boolean): Tone {
  if (!ok) return 'blocked';
  return missing ? 'warn' : 'ok';
}

function linkPillMark(ok: boolean, missing: boolean): string {
  if (!ok) return '✕';
  return missing ? '?' : '✓';
}

function forecastTierText(tier: string): string {
  switch (tier) {
    case 'hourly':
      return 'forecast hourly';
    case 'long-range':
      return 'long-range forecast';
    case 'off':
      return 'weather off · standard atmosphere';
    default:
      return 'beyond the forecast · standard atmosphere';
  }
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
  /** Weather mode: the route coloured by the weather, the weather card and tags; off by default */
  readonly weatherMode = signal(false);
  /** The weather value that colours the route in weather mode */
  readonly weatherMetric = signal<WeatherMetric>('headwind');
  /** Pre-flight weather briefing over the map */
  readonly briefingOpen = signal(false);
  readonly layersOpen = signal(false);
  readonly radioSetup = signal<LinkKind | null>(null);
  /** Link shown in the coverage heatmap */
  readonly coverageLink = signal<LinkKind>('control');
  /** The coverage shown in a mini-map in the corner instead of over the map (Layers › Coverage) */
  readonly coverageMiniMap = signal(false);
  /** Line of sight from the pilot to the point under the mouse while the result is read (Layers › Coverage) */
  readonly lineOfSight = signal(false);
  readonly forecastOffsetH = signal(0);

  readonly mission = this.store.mission;
  readonly shown = this.store.shownMission;
  readonly result = this.store.result;
  readonly calculated = computed(() => !!this.store.result());
  readonly evaluated = computed(() => linksEvaluated(this.mission()?.evaluate ?? 'both'));

  /** Coverage link actually shown (only evaluated links have a grid). */
  readonly heatLink = computed<LinkKind>(() => {
    const ev = this.evaluated();
    return ev.includes(this.coverageLink()) ? this.coverageLink() : ev[0];
  });

  readonly batteryFailure = computed(() => this.store.result()?.moduleFailures?.find((f) => (f.module ?? '').startsWith('fpv.battery')) ?? null);

  // ---------- reasons ----------
  /** Route-wide reasons (module reasons without a leg), each naming its module. */
  readonly moduleReasons = computed<Reason[]>(() => this.store.result()?.reasons ?? []);

  readonly batteryReason = computed(() => this.moduleReasons().find((r) => (r.module ?? '').startsWith('fpv.battery')) ?? null);

  // ---------- pills ----------
  readonly pills = computed<PillVm[]>(() => {
    const r = this.store.result();
    const m = this.mission();
    if (!r || !m) return [];
    const legs = r.legs ?? [];
    const missing = legs.some((l) => l.status === 'missing-data');
    const pills: PillVm[] = (['control', 'video'] as LinkKind[])
      .filter((kind) => this.evaluated().includes(kind))
      .map((kind) => this.linkPill(kind, legs, missing));
    const battery = this.batteryPill(r, m);
    if (battery) pills.push(battery);
    return pills;
  });

  /** Pill for one evaluated link: its worst margin along the route. */
  private linkPill(kind: LinkKind, legs: LegResult[], missing: boolean): PillVm {
    const letter = LINK_LETTER[kind];
    const margins = legs.map((l) => l.worst?.[kind]?.marginDb).filter((x): x is number => x !== undefined);
    if (margins.length === 0) return { key: kind, tone: 'warn', text: `${letter} ? not calculated` };
    const worst = Math.min(...margins);
    const ok = worst >= 0;
    return {
      key: kind,
      tone: linkPillTone(ok, missing),
      text: `${letter} ${linkPillMark(ok, missing)} ${RfFormat.format(worst, 'db+')}${missing && ok ? ' · part missing' : ''}`,
      title: `Worst ${kind} margin along the route: ${RfFormat.full(worst, 'db+')}`,
    };
  }

  /** Battery pill, or null when there is no battery verdict to show. */
  private batteryPill(r: RouteResult, m: Mission): PillVm | null {
    const b = r.battery;
    if (this.batteryFailure()) return { key: 'battery', tone: 'warn', text: 'Battery ? not checked', title: 'The battery module failed; the verdict is missing, not assumed fine.' };
    if (!b) return null;
    if ((b.shortfallM ?? 0) > 0) return { key: 'battery', tone: 'warn', text: `Battery ! ${RfFormat.format(b.shortfallM, 'km1')} short`, title: RfFormat.full(b.shortfallM, 'km3') + ' short' };
    if (endsAtTurnBack(m)) return { key: 'battery', tone: 'ok', text: 'Battery ✓ turn back at T' };
    return { key: 'battery', tone: 'ok', text: `Battery ✓ ${RfFormat.format(b.rangeM, 'km1')} range` };
  }

  // ---------- legs ----------
  /** Leg by its 1-based number in the UI. Legs come in route order; LegResult.index is 0-based. */
  legResult(r: RouteResult | null, n: number): LegResult | undefined {
    return r?.legs?.[n - 1];
  }

  /** Legs of the shown mission (preview mission while previewing). */
  readonly legRows = computed<LegRowVm[]>(() => {
    const m = this.shown();
    if (!m) return [];
    const previewing = !!this.store.preview();
    const r = previewing || this.store.resultStale() ? null : this.store.result();
    const pts: Pick<MissionPoint, 'lat' | 'lon'>[] = [m.pilot, ...m.waypoints];
    const rows: LegRowVm[] = m.waypoints.map((w, i) => {
      const index = i + 1;
      const leg = this.legResult(r, index);
      const len = leg?.lengthM ?? distanceM(pts[i], pts[i + 1]);
      const { mark, markLabel, tone, failing } = this.legMark(leg);
      const from = pointName(m, i - 1);
      const to = pointName(m, i);
      return {
        index,
        name: `${index} · ${from} → ${to}`,
        from,
        to,
        info: `${RfFormat.format(len, 'km1')} · ${RfFormat.format(MissionAltitude.height(m, w), 'm')}`,
        infoTitle: `${RfFormat.full(len, 'km3')}, ${RfFormat.full(MissionAltitude.height(m, w), 'm')} ${MissionAltitude.basis(m)}`,
        mark,
        markLabel,
        tone,
        failing,
        isReturn: false,
        lengthM: len,
        heightM: MissionAltitude.height(m, w),
      };
    });
    const last = m.waypoints.at(-1);
    if (m.returnToPilot !== false && last) {
      // The server adds the leg last → pilot when returnToPilot; it is evaluated like any other leg.
      const index = rows.length + 1;
      const leg = this.legResult(r, index);
      const back = leg?.lengthM ?? distanceM(last, m.pilot);
      const from = pointName(m, m.waypoints.length - 1);
      const to = pointName(m, -1);
      const v = this.legMark(leg);
      rows.push({
        index,
        name: `${index} · ${from} → ${to}, return`,
        from,
        to,
        info: `${RfFormat.format(back, 'km1')} · ${RfFormat.format(MissionAltitude.height(m, last), 'm')} → ${RfFormat.format(m.pilot.aglM, 'm')}`,
        infoTitle: `${RfFormat.full(back, 'km3')} back to the pilot`,
        ...v,
        isReturn: true,
        lengthM: back,
        heightM: MissionAltitude.height(m, last),
      });
    }
    return rows;
  });

  private legMark(leg: LegResult | undefined): Pick<LegRowVm, 'mark' | 'markLabel' | 'tone' | 'failing'> {
    const failing = (['control', 'video'] as LinkKind[]).filter((k) => leg?.worst?.[k]?.reachable === false);
    if (!leg) return { mark: '', markLabel: '', tone: 'none', failing };
    if (leg.status === 'missing-data') return { mark: '? no data', markLabel: 'No elevation data, not calculated', tone: 'warn', failing };
    if (failing.length) return { mark: `${failing.map((k) => LINK_LETTER[k]).join(' ')} ✕`, markLabel: `${failing.join(' and ')} link blocked`, tone: 'blocked', failing };
    if (leg.status === 'warn') return { mark: '!', markLabel: 'Warning', tone: 'warn', failing };
    return { mark: '✓', markLabel: 'Reachable', tone: 'ok', failing };
  }

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
    const legs = r.legs ?? [];
    for (let i = 0; i < legs.length; i++) {
      const leg = legs[i];
      const failing = (['video', 'control'] as LinkKind[]).filter((k) => leg.worst?.[k]?.reachable === false);
      if (leg.status !== 'blocked' && failing.length === 0) continue;
      const link = failing[0] ?? 'video';
      const reasons = leg.reasons ?? [];
      // a reason with a fix first, then the blocked reason for this link, then any blocked reason
      const reason =
        reasons.find((x) => x.fix && (!x.link || x.link === link)) ??
        reasons.find((x) => x.severity === 'blocked' && x.link === link) ??
        reasons.find((x) => x.severity === 'blocked') ??
        reasons[0];
      const lr = leg.worst?.[link];
      const idx = i + 1;
      const back = i >= m.waypoints.length;
      return {
        legIndex: idx,
        title: `Leg ${idx} · ${back ? pointName(m, m.waypoints.length - 1) : pointName(m, i - 1)} → ${back ? pointName(m, -1) : pointName(m, i)}`,
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

  readonly missingLeg = computed(() => {
    const legs = this.store.result()?.legs ?? [];
    const i = legs.findIndex((l) => l.status === 'missing-data');
    return i < 0 ? null : { leg: legs[i], number: i + 1 };
  });

  /** Missing-data areas and drone positions of every leg, for the map outline. */
  /** Paths without terrain data run from the pilot, so they are hidden once the pilot has moved. */
  readonly missingAreas = computed(() =>
    (this.store.pilotStale() ? [] : (this.store.result()?.legs ?? [])).filter((l) => l.missingData || l.missing?.length).map((l) => ({ area: l.missingData ?? null, positions: l.missing ?? [] })),
  );

  readonly allClearText = computed(() => {
    const r = this.store.result();
    const m = this.mission();
    if (!r || !m || this.verdict() || this.missingLeg()) return null;
    // a module's info line says more than the core's per-link margin lines
    const info = [...(r.legs ?? []).flatMap((l) => l.reasons ?? []), ...(r.reasons ?? [])].find((x) => x.severity === 'info' && x.text && x.module !== 'rf-core');
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
    const approx = calc ? '' : '≈ ';
    const rangeText = range !== null && range !== undefined ? approx + RfFormat.format(range, 'km1') : '—';
    return `${label} · ${rangeText}`;
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
    // the result says which tier it used; before a calculation, the horizon tells what to expect
    const r = this.store.result();
    const tier = r?.forecastTier ?? this.data.tierFor(planned ? m.time.utc! : null);
    const tierText = forecastTierText(tier);
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
    const m = this.mission();
    if (this.store.pilotStale()) return out;
    const targetLeg = m ? legs[m.waypoints.length - 1] : undefined;
    for (const leg of legs) {
      const lr = leg.worst?.video ?? leg.worst?.control;
      out.pilot ??= lr?.ends?.ground?.groundMslM;
    }
    // the target's ground from the leg that ends there, when its worst sample is at the end
    const tl = targetLeg?.worst?.video ?? targetLeg?.worst?.control;
    if (tl?.sample?.alongLegM !== undefined && targetLeg?.lengthM !== undefined && Math.abs(tl.sample.alongLegM - targetLeg.lengthM) < 1) out.target = tl.ends?.air?.groundMslM;
    return out;
  });

  resetToEmpty(): void {
    this.why.set(null);
    this.placing.set(null);
    this.editingRoute.set(false);
    this.briefingOpen.set(false);
    this.weatherMode.set(false);
    this.store.clear();
  }
}

export { MINUS };
