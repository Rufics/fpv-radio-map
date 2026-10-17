import { RouteResult } from '../models/api.models';
import { Mission, MissionPoint } from '../models/mission.models';
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

/**
 * Client-side KML and GPX export of a mission. Full precision numbers; heights are above ground
 * (the mission stores AGL; MSL comes from the server's DEM and is not guessed here).
 */
export class MissionExporter {
  constructor(private readonly generator = 'FPV Radio Map') {}

  private pointName(p: MissionPoint, i: number, count: number): string {
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

  toKml(mission: Mission, result?: RouteResult | null): ExportedFile {
    const wps = mission.waypoints;
    const placemark = (name: string, p: MissionPoint, style: string, extra = '') =>
      `    <Placemark>
      <name>${xmlEscape(name)}</name>
      <styleUrl>#${style}</styleUrl>
      <ExtendedData><Data name="aglM"><value>${n(p.aglM)}</value></Data>${extra}</ExtendedData>
      <Point><altitudeMode>relativeToGround</altitudeMode><coordinates>${n(p.lon)},${n(p.lat)},${n(p.aglM)}</coordinates></Point>
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
    const coords = [mission.pilot, ...wps].map((p) => `${n(p.lon)},${n(p.lat)},${n(p.aglM)}`);
    if (mission.returnToPilot !== false) coords.push(`${n(mission.pilot.lon)},${n(mission.pilot.lat)},${n(mission.pilot.aglM)}`);
    const content = `<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2">
  <Document>
    <name>${xmlEscape(mission.name)}</name>
    <description>Exported by ${xmlEscape(this.generator)}. Heights are above ground level.</description>
    <Style id="pilot"><IconStyle><color>ffbad035</color></IconStyle></Style>
    <Style id="waypoint"><IconStyle><color>fff2ece7</color></IconStyle></Style>
    <Style id="target"><IconStyle><color>ff24a5f5</color></IconStyle></Style>
    <Style id="route"><LineStyle><color>ffbad035</color><width>4</width></LineStyle></Style>
${placemark(mission.pilot.label ?? 'A · pilot', mission.pilot, 'pilot')}
${wps.map((p, i) => placemark(this.pointName(p, i, wps.length), p, i === wps.length - 1 ? 'target' : 'waypoint', legData(i))).join('\n')}
    <Placemark>
      <name>Route</name>
      <styleUrl>#route</styleUrl>
      <LineString><altitudeMode>relativeToGround</altitudeMode><coordinates>${coords.join(' ')}</coordinates></LineString>
    </Placemark>
  </Document>
</kml>
`;
    return { filename: `${this.safeFileName(mission.name)}.kml`, mime: 'application/vnd.google-earth.kml+xml', content };
  }

  toGpx(mission: Mission): ExportedFile {
    const wps = mission.waypoints;
    const ext = (p: MissionPoint) => `<extensions><rfmap:aglM>${n(p.aglM)}</rfmap:aglM></extensions>`;
    const wpt = (tag: 'wpt' | 'rtept', name: string, p: MissionPoint, sym?: string) =>
      `  <${tag} lat="${n(p.lat)}" lon="${n(p.lon)}"><name>${xmlEscape(name)}</name>${sym ? `<sym>${sym}</sym>` : ''}${ext(p)}</${tag}>`;
    const route = [wpt('rtept', mission.pilot.label ?? 'A', mission.pilot), ...wps.map((p, i) => wpt('rtept', this.pointName(p, i, wps.length), p))];
    if (mission.returnToPilot !== false) route.push(wpt('rtept', 'A · return', mission.pilot));
    const content = `<?xml version="1.0" encoding="UTF-8"?>
<gpx version="1.1" creator="${xmlEscape(this.generator)}" xmlns="http://www.topografix.com/GPX/1/1" xmlns:rfmap="https://rfmap.local/gpx/1">
  <metadata><name>${xmlEscape(mission.name)}</name><desc>Heights in rfmap:aglM are above ground level.</desc></metadata>
${wpt('wpt', mission.pilot.label ?? 'A · pilot', mission.pilot, 'Pilot')}
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
