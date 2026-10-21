import { FlightLogRow, FlightView, PlanFactRow, ReplayFormat, ReplayReading } from '@rfmap/ui-kit';

export interface KeyValueVm {
  label: string;
  value: string;
  sub?: string;
  tone?: 'warn' | 'blocked';
}

const LOW_CELL_V = 3.5;
const LOW_LINK_PCT = 30;

/** Display text of a flight replay, kept out of the templates so it can be tested. */
export class ReplayVm {
  /** "07:42 · 4.86 km · 1,840 mAh": what the flight was, in one line. */
  static headline(row: FlightLogRow): string {
    const s = row.summary;
    const parts = [ReplayFormat.clock(row.durationS)];
    if (s.distanceM !== undefined) parts.push(ReplayFormat.distance(s.distanceM));
    if (s.usedMah !== undefined) parts.push(`${Math.round(s.usedMah).toLocaleString('en-GB')} mAh`);
    return parts.join(' · ');
  }

  static title(row: FlightLogRow): string {
    return row.craft || row.fileName || 'Flight';
  }

  static firmware(row: FlightLogRow): string {
    const f = row.firmware;
    return [f.name, f.version].filter(Boolean).join(' ') || 'unknown firmware';
  }

  /** The flight summary list (design 01): only what the log could tell. */
  static summary(view: FlightLogRow): KeyValueVm[] {
    const s = view.summary;
    const out: KeyValueVm[] = [{ label: 'Flight time', value: ReplayFormat.clock(s.armedS || s.durationS) }];
    if (s.distanceM !== undefined) out.push({ label: 'Distance flown', value: ReplayFormat.distance(s.distanceM) });
    if (s.maxHomeDistM !== undefined) out.push({ label: 'Max from home', value: ReplayFormat.distance(s.maxHomeDistM) });
    if (s.maxAltM !== undefined) out.push({ label: 'Max altitude', value: `${Math.round(s.maxAltM)} m` });
    if (s.maxSpeedMps !== undefined) out.push({ label: 'Max speed', value: ReplayFormat.speed(s.maxSpeedMps) });
    if (s.usedMah !== undefined) {
      const wh = s.usedWh === undefined ? '' : ` · ${s.usedWh.toFixed(1)} Wh`;
      out.push({ label: 'Used', value: `${Math.round(s.usedMah).toLocaleString('en-GB')} mAh${wh}` });
    }
    if (s.minCellV !== undefined) {
      out.push({ label: 'Lowest cell', value: `${s.minCellV.toFixed(2)} V`, tone: s.minCellV < LOW_CELL_V ? 'warn' : undefined });
    }
    if (s.minLinkPct !== undefined) {
      const name = s.linkChannel === 'linkQualityPct' ? 'Lowest link quality' : 'Lowest RSSI';
      out.push({ label: name, value: `${Math.round(s.minLinkPct)} %`, tone: s.minLinkPct < LOW_LINK_PCT ? 'warn' : undefined });
    }
    return out;
  }

  /** The read-out at the replay time (design 01 tiles). */
  static readout(view: FlightView, r: ReplayReading): KeyValueVm[] {
    const v = r.values;
    const link = v.linkQualityPct ?? v.rssiPct;
    const out: KeyValueVm[] = [];
    if (view.channels['gps']) {
      out.push({ label: 'From home', value: ReplayFormat.distance(v.homeDistM) });
      out.push({ label: 'Ground speed', value: ReplayFormat.speed(v.speedMps) });
    }
    if (v.altM !== undefined) out.push({ label: 'Altitude', value: ReplayFormat.fixed(v.altM, 0, 'm'), sub: v.climbMps ? ReplayFormat.fixed(v.climbMps, 1, 'm/s') : undefined });
    if (v.cellV !== undefined || v.voltageV !== undefined) {
      const used = v.usedMah === undefined || v.usedMah === null ? '' : ` · ${Math.round(v.usedMah).toLocaleString('en-GB')} mAh`;
      out.push({
        label: 'Battery',
        value: v.cellV === undefined ? ReplayFormat.fixed(v.voltageV, 2, 'V') : ReplayFormat.fixed(v.cellV, 2, 'V'),
        sub: v.cellV === undefined ? undefined : `${ReplayFormat.fixed(v.voltageV, 1, 'V')}${used}`,
        tone: v.cellV !== undefined && v.cellV !== null && v.cellV < LOW_CELL_V ? 'warn' : undefined,
      });
    }
    if (link !== undefined) {
      out.push({ label: v.linkQualityPct === undefined ? 'RSSI' : 'Link quality', value: ReplayFormat.fixed(link, 0, '%'), tone: link !== null && link < LOW_LINK_PCT ? 'warn' : undefined });
    }
    if (v.sats !== undefined) out.push({ label: 'GPS', value: v.sats === null ? 'no fix' : `${Math.round(v.sats)} sats` });
    if (r.band) {
      out.push({ label: 'Mode', value: ReplayFormat.band(r.band), sub: v.throttlePct === undefined ? undefined : `throttle ${ReplayFormat.fixed(v.throttlePct, 0, '%')}` });
    }
    return out;
  }

  /** The logbook head (design 10): how much was flown. */
  static totals(rows: FlightLogRow[]): KeyValueVm[] {
    const flights = rows.filter((r) => r.status !== 'header-only' && r.status !== 'unsupported');
    const air = flights.reduce((a, r) => a + (r.summary.armedS || 0), 0);
    const km = flights.reduce((a, r) => a + (r.summary.distanceM ?? 0), 0);
    const failsafes = rows.filter((r) => r.warnings.some((w) => w.startsWith('Failsafe'))).length;
    const h = Math.floor(air / 3600);
    const m = Math.round((air % 3600) / 60);
    return [
      { label: flights.length === 1 ? 'flight' : 'flights', value: String(flights.length) },
      { label: 'in the air', value: h > 0 ? `${h} h ${m} m` : `${m} m` },
      { label: 'flown', value: ReplayFormat.distance(km) },
      { label: failsafes === 1 ? 'flight with a failsafe' : 'flights with a failsafe', value: String(failsafes), tone: failsafes ? 'warn' : undefined },
    ];
  }

  /** "No flight data, header only" and the like: why a logbook row has nothing to replay. */
  static emptyReason(row: FlightLogRow): string | null {
    if (row.status === 'header-only') return 'No flight data, header only';
    if (row.status === 'unsupported') return 'Format not supported';
    return null;
  }

  /** A plan-against-fact value in its unit. */
  static planValue(row: PlanFactRow, which: 'plan' | 'fact'): string {
    const v = row[which];
    if (v === null || v === undefined) return '–';
    switch (row.metric) {
      case 'distance':
      case 'maxOffRoute':
        return row.metric === 'maxOffRoute' && which === 'plan' ? '–' : ReplayFormat.distance(v);
      case 'energy':
        return `${v.toFixed(1)} Wh`;
      case 'whPerKm':
        return `${v.toFixed(1)} Wh/km`;
      default:
        return which === 'plan' ? `${v >= 0 ? '+' : '−'}${Math.abs(v).toFixed(1)} dB` : `${Math.round(v)} %`;
    }
  }

  static planDelta(row: PlanFactRow): string {
    if (row.deltaPct === undefined) return '';
    return `${row.deltaPct >= 0 ? '+' : '−'}${Math.abs(Math.round(row.deltaPct))} %`;
  }
}
