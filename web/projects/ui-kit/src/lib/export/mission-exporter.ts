import { RouteResult } from '../models/api.models';
import { Mission, MissionAltitude, MissionWaypoint } from '../models/mission.models';
import { RfFormat } from '../format/rf-format';

export function xmlEscape(s: string): string {
  return s.replaceAll(/[<>&'"]/g, (c) => ({ '<': '&lt;', '>': '&gt;', '&': '&amp;', "'": '&apos;', '"': '&quot;' })[c] as string);
}

const n = (v: number) => RfFormat.exportNumber(v);

export interface ExportedFile {
  filename: string;
  mime: string;
  content: string;
}

/** A route point as exported: its height and the field that height belongs to. */
interface ExportPoint {
  lat: number;
  lon: number;
  label?: string;
  heightM: number;
  field: 'aglM' | 'altM';
}

/**
 * Client-side KML and GPX export of a mission. Full precision numbers. Heights are written in the
 * mission's own reference: above ground (KML relativeToGround) or above the launch point (rfmap altM,
 * which KML cannot express, so those points are clamped to the ground). MSL comes from the server's
 * DEM and is not guessed here; the server-side export (fpv.core) writes altitudes above sea level.
 */
export class MissionExporter {
  constructor(private readonly generator = 'FPV Radio Map') {}

  private pointName(p: MissionWaypoint, i: number, count: number): string {
    if (p.label) return p.label;
    return i === count - 1 ? 'B · target' : `WP${i + 1}`;
  }

  private safeFileName(name: string): string {
    const dashed = (name.trim() || 'mission').replaceAll(/[^\p{L}\p{N}._-]+/gu, '-');
    let start = 0;
    let end = dashed.length;
    while (start < end && dashed[start] === '-') start++;
    while (end > start && dashed[end - 1] === '-') end--;
    return dashed.slice(start, end).toLowerCase() || 'mission';
  }

  /** The pilot (antenna above ground) and the waypoints (in the mission's reference). */
  private points(mission: Mission): { pilot: ExportPoint; waypoints: ExportPoint[] } {
    const field = MissionAltitude.field(mission);
    const p = mission.pilot;
    return {
      pilot: { lat: p.lat, lon: p.lon, label: p.label, heightM: p.aglM, field: 'aglM' },
      waypoints: mission.waypoints.map((w) => ({ lat: w.lat, lon: w.lon, label: w.label, heightM: MissionAltitude.height(mission, w), field })),
    };
  }

  private heightsNote(mission: Mission): string {
    return MissionAltitude.ref(mission) === 'launch'
      ? 'Waypoint heights (altM) are above the launch point; the pilot antenna height (aglM) is above ground.'
      : 'Heights are above ground level.';
  }

  toKml(mission: Mission, result?: RouteResult | null): ExportedFile {
    const { pilot, waypoints: wps } = this.points(mission);
    const mode = (p: ExportPoint) => (p.field === 'aglM' ? 'relativeToGround' : 'clampToGround');
    const placemark = (name: string, p: ExportPoint, style: string, extra = '') =>
      `    <Placemark>
      <name>${xmlEscape(name)}</name>
      <styleUrl>#${style}</styleUrl>
      <ExtendedData><Data name="${p.field}"><value>${n(p.heightM)}</value></Data>${extra}</ExtendedData>
      <Point><altitudeMode>${mode(p)}</altitudeMode><coordinates>${n(p.lon)},${n(p.lat)},${n(p.heightM)}</coordinates></Point>
    </Placemark>`;
    const legData = (i: number) => {
      const leg = result?.legs?.[i]; // legs are in route order (LegResult.index is 0-based on the server)
      if (!leg) return '';
      const parts: string[] = [`<Data name="status"><value>${xmlEscape(leg.status ?? '')}</value></Data>`];
      for (const kind of ['control', 'video'] as const) {
        const m = leg.worst?.[kind]?.marginDb;
        if (m !== undefined) parts.push(`<Data name="${kind}MarginDb"><value>${n(m)}</value></Data>`);
      }
      return parts.join('');
    };
    const coords = [pilot, ...wps].map((p) => `${n(p.lon)},${n(p.lat)},${n(p.heightM)}`);
    if (mission.returnToPilot !== false) coords.push(`${n(pilot.lon)},${n(pilot.lat)},${n(pilot.heightM)}`);
    const lineMode = MissionAltitude.ref(mission) === 'launch' ? 'clampToGround' : 'relativeToGround';
    const content = `<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2">
  <Document>
    <name>${xmlEscape(mission.name)}</name>
    <description>Exported by ${xmlEscape(this.generator)}. ${this.heightsNote(mission)}</description>
    <Style id="pilot"><IconStyle><color>ffbad035</color></IconStyle></Style>
    <Style id="waypoint"><IconStyle><color>fff2ece7</color></IconStyle></Style>
    <Style id="target"><IconStyle><color>ff24a5f5</color></IconStyle></Style>
    <Style id="route"><LineStyle><color>ffbad035</color><width>4</width></LineStyle></Style>
${placemark(pilot.label ?? 'A · pilot', pilot, 'pilot')}
${wps.map((p, i) => placemark(this.pointName(p, i, wps.length), p, i === wps.length - 1 ? 'target' : 'waypoint', legData(i))).join('\n')}
    <Placemark>
      <name>Route</name>
      <styleUrl>#route</styleUrl>
      <LineString><altitudeMode>${lineMode}</altitudeMode><coordinates>${coords.join(' ')}</coordinates></LineString>
    </Placemark>
  </Document>
</kml>
`;
    return { filename: `${this.safeFileName(mission.name)}.kml`, mime: 'application/vnd.google-earth.kml+xml', content };
  }

  toGpx(mission: Mission): ExportedFile {
    const { pilot, waypoints: wps } = this.points(mission);
    const ext = (p: ExportPoint) => `<extensions><rfmap:${p.field}>${n(p.heightM)}</rfmap:${p.field}></extensions>`;
    const wpt = (tag: 'wpt' | 'rtept', name: string, p: ExportPoint, sym?: string) => {
      const symTag = sym ? `<sym>${sym}</sym>` : '';
      return `  <${tag} lat="${n(p.lat)}" lon="${n(p.lon)}"><name>${xmlEscape(name)}</name>${symTag}${ext(p)}</${tag}>`;
    };
    const route = [wpt('rtept', pilot.label ?? 'A', pilot), ...wps.map((p, i) => wpt('rtept', this.pointName(p, i, wps.length), p))];
    if (mission.returnToPilot !== false) route.push(wpt('rtept', 'A · return', pilot));
    const desc = MissionAltitude.ref(mission) === 'launch'
      ? 'Heights in rfmap:altM are above the launch point; rfmap:aglM (the pilot) is above ground level.'
      : 'Heights in rfmap:aglM are above ground level.';
    const content = `<?xml version="1.0" encoding="UTF-8"?>
<gpx version="1.1" creator="${xmlEscape(this.generator)}" xmlns="http://www.topografix.com/GPX/1/1" xmlns:rfmap="https://rfmap.local/gpx/1">
  <metadata><name>${xmlEscape(mission.name)}</name><desc>${desc}</desc></metadata>
${wpt('wpt', pilot.label ?? 'A · pilot', pilot, 'Pilot')}
${wps.map((p, i) => wpt('wpt', this.pointName(p, i, wps.length), p, i === wps.length - 1 ? 'Target' : 'Waypoint')).join('\n')}
  <rte>
    <name>${xmlEscape(mission.name)}</name>
${route.map((r) => '  ' + r).join('\n')}
  </rte>
</gpx>
`;
    return { filename: `${this.safeFileName(mission.name)}.gpx`, mime: 'application/gpx+xml', content };
  }

  /** Give the viewer the file to save. */
  static download(file: ExportedFile): void {
    const blob = new Blob([file.content], { type: file.mime });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = file.filename;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
}
