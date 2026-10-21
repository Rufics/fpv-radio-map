/**
 * DEV MOCK ONLY — flight logs for the mock backend. The browser cannot decode Blackbox files the
 * way the server's fpv.blackbox module does, so the mock answers a synthetic flight along the
 * mission's route instead, in the same flightview/v1 shape. A file named like the header-only
 * sample (btfl_016) or smaller than 16 KB answers a log with no flight data.
 */
import {
  FlightLogRow,
  FlightModeBand,
  FlightStoryEvent,
  FlightView,
  LatLon,
  Mission,
  PlanFact,
  RouteResult,
  destinationPoint,
  distanceM,
} from '@rfmap/ui-kit';

const HZ = 5;
const SPEED_MPS = 13;
const CLIMB_S = 20;
const CRUISE_AGL_M = 80;

interface Sample {
  t: number;
  p: LatLon;
  alongM: number;
}

export class MockFlights {
  /** The flight a file would hold, or a header-only log for the header-only sample. */
  static decode(fileName: string, bytes: number, mission: Mission | null): FlightView[] {
    if (bytes < 16 * 1024 || /016/.test(fileName)) return [MockFlights.headerOnly(fileName)];
    return [MockFlights.flight(fileName, mission)];
  }

  static row(v: FlightView): FlightLogRow {
    const { id, index, status, durationS, firmware, craft, startedAt, summary, bands, warnings, fileName, decoder } = v;
    return { id, index, status, durationS, firmware, craft, startedAt, summary, bands, warnings, fileName, decoder };
  }

  static headerOnly(fileName: string): FlightView {
    return {
      schema: 'flightview/v1',
      index: 1,
      status: 'header-only',
      durationS: 0,
      firmware: { name: 'Betaflight', version: '4.5.1', supported: true },
      craft: 'LR4-O4PRO',
      startedAt: '2025-02-02T15:15:20.000+00:00',
      summary: { durationS: 0, armedS: 0 },
      bands: [],
      warnings: [],
      fileName,
      decoder: 'fpv.blackbox',
      channels: {},
      notes: [
        {
          code: 'header-only',
          severity: 'warn',
          message: 'Only the header was written, then the log ended.',
          fix: 'Check that the craft stayed armed and that Blackbox logging is on.',
        },
      ],
      homes: [],
      story: [],
      timeline: { hz: HZ, t: [0], channels: {} },
    };
  }

  /** Along the route at cruise speed, with a weak-link stretch and a short failsafe at the far end. */
  static flight(fileName: string, mission: Mission | null): FlightView {
    const route = MockFlights.route(mission);
    const samples = MockFlights.samples(route);
    const n = samples.length;
    const durationS = samples[n - 1].t;
    const lengthM = samples[n - 1].alongM;
    const home = route[0];
    const far = samples.reduce((a, s) => (distanceM(home, s.p) > distanceM(home, a.p) ? s : a), samples[0]);
    const ch: Record<string, (number | null)[]> = {
      lat: [], lon: [], altM: [], homeDistM: [], distanceM: [], speedMps: [], climbMps: [], voltageV: [], cellV: [],
      currentA: [], usedMah: [], usedWh: [], linkQualityPct: [], rssiDbm: [], throttlePct: [], sats: [], gpsFix: [], armed: [],
    };
    let mah = 0;
    let wh = 0;
    for (const s of samples) {
      const climb = Math.min(1, s.t / CLIMB_S) * Math.min(1, (durationS - s.t) / CLIMB_S);
      const alt = CRUISE_AGL_M * climb + 6 * Math.sin(s.t / 9);
      const fromHome = distanceM(home, s.p);
      const amps = 6 + 18 * climb + 3 * Math.sin(s.t / 5);
      const pack = 16.6 - (2.5 * s.t) / durationS - amps * 0.02;
      mah += (amps * (1 / HZ)) / 3.6;
      wh += (pack * amps * (1 / HZ)) / 3600;
      const nearFar = Math.abs(s.t - far.t);
      const lq = Math.max(0, Math.min(100, 100 - fromHome / 40 - (nearFar < 6 ? 60 - nearFar * 8 : 0)));
      ch['lat'].push(s.p.lat);
      ch['lon'].push(s.p.lon);
      ch['altM'].push(alt);
      ch['homeDistM'].push(fromHome);
      ch['distanceM'].push(s.alongM);
      ch['speedMps'].push(s.t < 3 || s.t > durationS - 3 ? 0 : SPEED_MPS + 2 * Math.sin(s.t / 4));
      ch['climbMps'].push(0);
      ch['voltageV'].push(pack);
      ch['cellV'].push(pack / 4);
      ch['currentA'].push(amps);
      ch['usedMah'].push(mah);
      ch['usedWh'].push(wh);
      ch['linkQualityPct'].push(lq);
      ch['rssiDbm'].push(-50 - fromHome / 30);
      ch['throttlePct'].push(30 + 25 * climb);
      ch['sats'].push(15);
      ch['gpsFix'].push(1);
      ch['armed'].push(1);
    }
    const bands: FlightModeBand[] = [
      { from: 0, to: 30, band: 'ANGLE' },
      { from: 30, to: far.t, band: 'ACRO' },
      { from: far.t, to: far.t + 0.4, band: 'FAILSAFE' },
      { from: far.t + 0.4, to: durationS - 20, band: 'ACRO' },
      { from: durationS - 20, to: durationS, band: 'ANGLE' },
    ];
    const at = (t: number) => samples[Math.min(n - 1, Math.round(t * HZ))].p;
    const event = (t: number, type: string, severity: FlightStoryEvent['severity'], title: string, detail?: string): FlightStoryEvent => ({
      t, type, severity, title, detail, ...at(t),
    });
    const lowCellT = durationS * 0.8;
    const story: FlightStoryEvent[] = [
      event(0, 'armed', 'info', 'Armed', '15 sats · 16.6 V · home set'),
      event(30, 'mode', 'info', 'Mode: Acro', 'from Angle'),
      event(far.t - 4, 'link-low', 'warn', 'Weak link: Link quality low', 'below 30 % for 4.0 s, lowest 0 %'),
      event(far.t, 'failsafe', 'blocked', 'Failsafe', 'receiver signal lost, failsafe started'),
      event(far.t + 0.4, 'failsafe', 'info', 'Failsafe over'),
      event(far.t + 1, 'farthest', 'info', 'Farthest point', `${(distanceM(home, far.p) / 1000).toFixed(2)} km from home`),
      event(lowCellT, 'low-cell', 'warn', 'Battery low', 'cells under 3.50 V for 2 s'),
      event(durationS - 20, 'mode', 'info', 'Mode: Angle', 'from Acro'),
      event(durationS, 'disarmed', 'info', 'Disarmed', 'disarmed with the arm switch'),
    ];
    const summary = {
      durationS,
      armedS: durationS,
      cells: 4,
      distanceM: lengthM,
      maxHomeDistM: Math.max(...(ch['homeDistM'] as number[])),
      maxAltM: CRUISE_AGL_M + 6,
      maxSpeedMps: SPEED_MPS + 2,
      maxCurrentA: 27,
      usedMah: mah,
      usedWh: wh,
      startVoltageV: 16.6,
      minVoltageV: Math.min(...(ch['voltageV'] as number[])),
      minCellV: Math.min(...(ch['cellV'] as number[])),
      minLinkPct: 0,
      linkChannel: 'linkQualityPct' as const,
      disarmReason: 'SWITCH',
    };
    return {
      schema: 'flightview/v1',
      index: 1,
      status: 'ok',
      durationS,
      firmware: { name: 'Betaflight', version: '4.5.1', revision: '77d01ba3b', target: 'STM32F7X2', supported: true },
      craft: 'LR4-O4PRO',
      startedAt: '2025-02-02T15:15:20.000+00:00',
      summary,
      bands,
      warnings: story.filter((e) => e.severity !== 'info').map((e) => e.title),
      fileName,
      decoder: 'fpv.blackbox',
      channels: { gps: true, voltage: true, current: true, rssi: false, linkQuality: true, rssiDbm: true, throttle: true, modes: true, baroAltitude: false },
      notes: [],
      radio: { protocol: 'CRSF' },
      homes: [{ t: 0, ...home }],
      story,
      timeline: { hz: HZ, t: samples.map((s) => s.t), channels: ch },
    };
  }

  /** Plan against fact for the mock flight: the mock flew the route, so it stays close to it. */
  static planFact(view: FlightView, mission: Mission, result: RouteResult | undefined): PlanFact {
    const route = MockFlights.route(mission);
    let plannedM = 0;
    for (let i = 1; i < route.length; i++) plannedM += distanceM(route[i - 1], route[i]);
    const neededWh = (result?.modules?.['fpv.battery'] as { neededWh?: number } | undefined)?.neededWh ?? null;
    const usedWh = view.summary.usedWh ?? null;
    const legs = route.slice(1).map((p, i) => ({
      index: i,
      from: i === 0 ? 'Pilot' : `WP${i}`,
      to: i === route.length - 2 ? 'Pilot' : `WP${i + 1}`,
      lengthM: distanceM(route[i], p),
      controlMarginDb: result?.legs?.[i]?.worst?.control?.marginDb ?? null,
      videoMarginDb: result?.legs?.[i]?.worst?.video?.marginDb ?? null,
      flownS: distanceM(route[i], p) / SPEED_MPS,
      minLinkPct: i === 0 ? 0 : 70,
      maxOffRouteM: 18 + 4 * i,
    }));
    const flown = view.summary.distanceM ?? null;
    return {
      evaluated: !!result,
      rows: [
        { metric: 'distance', label: 'Distance', unit: 'm', plan: plannedM, fact: flown, deltaPct: flown ? ((flown - plannedM) / plannedM) * 100 : undefined },
        { metric: 'energy', label: 'Energy', unit: 'Wh', plan: neededWh, fact: usedWh, deltaPct: neededWh && usedWh ? ((usedWh - neededWh) / neededWh) * 100 : undefined },
        { metric: 'whPerKm', label: 'Energy per km', unit: 'Wh/km', plan: neededWh ? neededWh / (plannedM / 1000) : null, fact: usedWh && flown ? usedWh / (flown / 1000) : null },
        { metric: 'weakestLink', label: 'Weakest control link', unit: 'dB / LQ %', plan: legs[0]?.controlMarginDb ?? null, fact: 0 },
        { metric: 'maxOffRoute', label: 'Furthest off the route', unit: 'm', plan: 0, fact: 18 + 4 * (legs.length - 1) },
      ],
      legs,
      findings: [
        { severity: 'info', text: `Flew ${((flown ?? 0) / 1000).toFixed(2)} km against ${(plannedM / 1000).toFixed(2)} km planned.` },
        { severity: 'warn', text: 'Leg Pilot → WP1: the link fell to 0 % at the far end. Check antennas, obstacles and the radio settings.' },
      ],
      offRouteM: view.timeline.t.map((_, i) => 6 + 12 * Math.abs(Math.sin(i / 40))),
    };
  }

  /** Pilot, waypoints, and back to the pilot; a small square when there is no mission. */
  private static route(mission: Mission | null): LatLon[] {
    if (!mission) {
      const a = { lat: 48.5, lon: 35.2 };
      return [a, destinationPoint(a.lat, a.lon, 45, 900), destinationPoint(a.lat, a.lon, 100, 1200), a];
    }
    const pts = [mission.pilot, ...mission.waypoints].map((p) => ({ lat: p.lat, lon: p.lon }));
    if (mission.returnToPilot !== false) pts.push({ lat: mission.pilot.lat, lon: mission.pilot.lon });
    return pts;
  }

  private static samples(route: LatLon[]): Sample[] {
    const out: Sample[] = [];
    let t = 0;
    let along = 0;
    for (let i = 1; i < route.length; i++) {
      const a = route[i - 1];
      const b = route[i];
      const len = distanceM(a, b);
      const steps = Math.max(1, Math.round((len / SPEED_MPS) * HZ));
      for (let k = 0; k < steps; k++) {
        const f = k / steps;
        out.push({ t, p: { lat: a.lat + (b.lat - a.lat) * f, lon: a.lon + (b.lon - a.lon) * f }, alongM: along + len * f });
        t += 1 / HZ;
      }
      along += len;
    }
    out.push({ t, p: route.at(-1) ?? route[0], alongM: along });
    return out;
  }
}
