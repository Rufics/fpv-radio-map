import { LinkKind, Mission, MissionExporter, MissionPoint, applyMergePatch, distanceM, interpolate, linksEvaluated, sampleGrid, xmlEscape } from '@rfmap/ui-kit';
import type { FpvComparePoint, FpvCompareData, FpvCoreData, FpvCoreOptions } from '../core/fpv-core';
import { coverageGrid, routeResult } from './reference-case';
import { aglPoint, elevation } from './mock-terrain';

/** Points every `stepM` along pilot → waypoints, with the AGL interpolated per leg. */
function samples(m: Mission, stepM: number): MissionPoint[] {
  const pts = [m.pilot, ...m.waypoints.map((w) => aglPoint(m, w))];
  const out: MissionPoint[] = [];
  for (let i = 0; i + 1 < pts.length; i++) {
    const a = pts[i];
    const b = pts[i + 1];
    const n = Math.max(1, Math.ceil(distanceM(a, b) / stepM));
    for (let k = i === 0 ? 0 : 1; k <= n; k++) {
      const t = k / n;
      const p = interpolate(a, b, t);
      out.push({ lat: p.lat, lon: p.lon, aglM: a.aglM + (b.aglM - a.aglM) * t });
    }
  }
  return out;
}

const sameGeometry = (a: Mission, b: Mission) => {
  const pa = [a.pilot, ...a.waypoints];
  const pb = [b.pilot, ...b.waypoints];
  return pa.length === pb.length && pa.every((p, i) => Math.abs(p.lat - pb[i].lat) < 1e-9 && Math.abs(p.lon - pb[i].lon) < 1e-9);
};

/**
 * Mock of modules/fpv/core: data.compare (scenario B = merge patch) and data.export.
 * Margins along the route come from the mock coverage grid (drone height of the grid), so A and B
 * differ exactly by what the patch changes in the link budget. Returns the module's `data`, which the
 * server puts at RouteResult.modules['fpv.core'] as is (its reasons, if any, go to RouteResult.reasons).
 */
export function fpvCoreMock(mission: Mission): FpvCoreData {
  const opts = (mission.modules?.['fpv.core'] as FpvCoreOptions | undefined) ?? {};
  const data: FpvCoreData = {};
  if (opts.compare) data.compare = compareData(mission, opts.compare, opts.stepM ?? 100);
  if (opts.export?.length) data.export = exportData(mission, opts);
  return data;
}

/** data.compare: route A (the mission) against B (the mission with the merge patch applied). */
function compareData(mission: Mission, compare: NonNullable<FpvCoreOptions['compare']>, stepM: number): FpvCompareData {
  const b = applyMergePatch(structuredClone(mission), compare.patch);
  const aligned = sameGeometry(mission, b);
  const out: FpvCompareData = { name: compare.name ?? 'B', mission: b, aligned, stepM, links: {} };
  for (const kind of linksEvaluated(mission.evaluate) as LinkKind[]) {
    out.links![kind] = compareLink(mission, b, kind, stepM, aligned);
  }
  return out;
}

function compareLink(mission: Mission, b: Mission, kind: LinkKind, stepM: number, aligned: boolean): NonNullable<FpvCompareData['links']>[LinkKind] {
  const ga = coverageGrid(mission, kind);
  const gb = coverageGrid(b, kind);
  const sa = samples(mission, stepM);
  const sb = samples(b, stepM);
  // anchor the grid samples to the route result, so A's worst margin equals the route verdict
  const routeWorst = Math.min(...(routeResult(mission, 'normal').legs ?? []).map((l) => l.worst?.[kind]?.marginDb ?? Infinity));
  const rawA = sa.map((p) => sampleGrid(ga, p.lon, p.lat) ?? -99);
  const shift = Number.isFinite(routeWorst) ? routeWorst - Math.min(...rawA) : 0;
  const ma = rawA.map((v) => v + shift);
  const mb = sb.map((p) => (sampleGrid(gb, p.lon, p.lat) ?? -99) + shift);
  const worst = (v: number[]) => v.reduce((w, x, i) => (x < w.worstMarginDb ? { worstMarginDb: x, sample: i } : w), { worstMarginDb: Infinity, sample: 0 });
  const wa = worst(ma);
  const wb = worst(mb);
  const link: NonNullable<FpvCompareData['links']>[LinkKind] = { a: wa, b: wb, deltaWorstDb: wb.worstMarginDb - wa.worstMarginDb };
  if (aligned) {
    const points: FpvComparePoint[] = sa.map((p, i) => ({ lat: p.lat, lon: p.lon, aMarginDb: ma[i], bMarginDb: mb[i], deltaDb: mb[i] - ma[i] }));
    link.points = points;
    link.pointsBetter = points.filter((p) => p.deltaDb > 1e-9).length;
    link.pointsWorse = points.filter((p) => p.deltaDb < -1e-9).length;
  }
  return link;
}

/** data.export: the requested files (GPX, KML) for the mission. */
function exportData(mission: Mission, opts: FpvCoreOptions): NonNullable<FpvCoreData['export']> {
  const exported: NonNullable<FpvCoreData['export']> = {};
  const step = opts.exportStepM ?? 50;
  const ex = new MissionExporter();
  for (const f of opts.export ?? []) {
    if (f === 'gpx') exported.gpx = { mediaType: 'application/gpx+xml', content: ex.toGpx(mission).content };
    else exported.kml = { mediaType: 'application/vnd.google-earth.kml+xml', content: kml(mission, step, opts.kmlAltitude ?? 'relativeToGround') };
  }
  return exported;
}

function kml(m: Mission, stepM: number, mode: 'relativeToGround' | 'absolute'): string {
  const alt = (p: MissionPoint) => (mode === 'absolute' ? elevation(p.lat, p.lon) + p.aglM : p.aglM);
  const coord = (p: MissionPoint) => `${p.lon.toFixed(7)},${p.lat.toFixed(7)},${alt(p).toFixed(1)}`;
  const pts = [m.pilot, ...m.waypoints.map((w) => aglPoint(m, w))];
  const marks = pts
    .map((p, i) => {
      const name = p.label ?? (i === 0 ? 'A' : `WP${i}`);
      return `    <Placemark><name>${xmlEscape(name)}</name><Point><altitudeMode>${mode}</altitudeMode><coordinates>${coord(p)}</coordinates></Point></Placemark>`;
    })
    .join('\n');
  return `<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2">
  <Document>
    <name>${xmlEscape(m.name)}</name>
    <description>Exported by fpv.core (mock). Heights ${mode === 'absolute' ? 'above sea level (DEM + AGL)' : 'above ground'}; route sampled every ${stepM} m.</description>
${marks}
    <Placemark><name>Route</name><LineString><altitudeMode>${mode}</altitudeMode><coordinates>${samples(m, stepM).map(coord).join(' ')}</coordinates></LineString></Placemark>
  </Document>
</kml>
`;
}
