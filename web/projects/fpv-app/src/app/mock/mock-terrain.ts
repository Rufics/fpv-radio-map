/**
 * DEV MOCK ONLY. A synthetic terrain around the reference case near Dnipro (48.53 N, 35.20 E):
 * a NW–SE ridge 2.6 km from the pilot towards the target, peaking at 622 m on the A→B line,
 * a river valley to the south. Used for the mock DEM / satellite / weather tiles, contours,
 * the mock base map and the mock coverage grids. The real app gets all of this from the server.
 */
import { contours } from 'd3-contour';
import type { Feature, FeatureCollection, LineString, MultiPolygon, Position } from 'geojson';

export const ORIGIN = { lat: 48.53, lon: 35.2 };
const KM_PER_DEG_LAT = 111.195;
const KM_PER_DEG_LON = KM_PER_DEG_LAT * Math.cos((ORIGIN.lat * Math.PI) / 180);

export function toLatLon(xKm: number, yKm: number): { lat: number; lon: number } {
  return { lat: ORIGIN.lat + yKm / KM_PER_DEG_LAT, lon: ORIGIN.lon + xKm / KM_PER_DEG_LON };
}
export function toLocal(lat: number, lon: number): { x: number; y: number } {
  return { x: (lon - ORIGIN.lon) * KM_PER_DEG_LON, y: (lat - ORIGIN.lat) * KM_PER_DEG_LAT };
}

/** Route in local km (A = pilot at the origin). Lengths: 3.683 / 3.295 / 3.866 km; A→B 5.280 km. */
export const LOCAL = {
  A: { x: 0, y: 0 },
  WP1: { x: -2.08, y: 3.04 },
  WP2: { x: 0.8, y: 4.64 },
  B: { x: 4.3222, y: 3.0331 },
};
const AB = Math.hypot(LOCAL.B.x, LOCAL.B.y);
const U = { x: LOCAL.B.x / AB, y: LOCAL.B.y / AB };
export const RIDGE_FRACTION = 2.6 / AB;
export const RIDGE = { x: LOCAL.B.x * RIDGE_FRACTION, y: LOCAL.B.y * RIDGE_FRACTION };

const soft = (v: number, lim = 15) => lim * Math.tanh(v / lim);

function baseSlope(x: number, y: number): number {
  return 544 - 30 * soft(x) - 12 * soft(y);
}
const RIDGE_H = 622 - baseSlope(RIDGE.x, RIDGE.y);

/** Ground elevation (m MSL) at local km coordinates. */
export function elevationLocal(x: number, y: number): number {
  const dx = x - RIDGE.x;
  const dy = y - RIDGE.y;
  const across = dx * U.x + dy * U.y;
  const along = -dx * U.y + dy * U.x;
  const ridge = RIDGE_H * Math.exp(-((across / 0.55) ** 2)) * Math.exp(-((along / 2.2) ** 2));
  const ridge2 = 70 * Math.exp(-(((x + 5.5) / 2.4) ** 2) - ((y - 6.5) / 1.6) ** 2);
  const riverY = -3.2 + 0.25 * x + 0.6 * Math.sin(x / 2.3);
  const river = -75 * Math.exp(-(((y - riverY) / 1.1) ** 2));
  const rA = x * x + y * y;
  const rB = (x - LOCAL.B.x) ** 2 + (y - LOCAL.B.y) ** 2;
  const g = (1 - Math.exp(-rA / 0.4)) * (1 - Math.exp(-rB / 0.4));
  const hills = g * (9 * Math.sin(1.3 * x + 0.7) * Math.cos(1.1 * y - 0.4) + 5 * Math.sin(2.7 * x - 1.9 * y) + 3 * Math.cos(4.1 * x + 3.3 * y));
  return baseSlope(x, y) + ridge + ridge2 + river + hills;
}

export function elevation(lat: number, lon: number): number {
  const p = toLocal(lat, lon);
  return elevationLocal(p.x, p.y);
}

// ---------------- tiles ----------------
function tileToLonLat(z: number, x: number, y: number): { lon: number; lat: number } {
  const n = 2 ** z;
  const lon = (x / n) * 360 - 180;
  const lat = (Math.atan(Math.sinh(Math.PI * (1 - (2 * y) / n))) * 180) / Math.PI;
  return { lon, lat };
}

function parseZxy(url: string): [number, number, number] {
  const m = url.match(/(\d+)\/(\d+)\/(\d+)/);
  if (!m) throw new Error(`bad tile url ${url}`);
  return [Number(m[1]), Number(m[2]), Number(m[3])];
}

async function canvasToBuffer(c: OffscreenCanvas): Promise<ArrayBuffer> {
  const blob = await c.convertToBlob({ type: 'image/png' });
  return blob.arrayBuffer();
}

function forEachPixel(z: number, tx: number, ty: number, size: number, fn: (i: number, lat: number, lon: number) => void): void {
  for (let py = 0; py < size; py++) {
    const { lat } = tileToLonLat(z, tx, ty + (py + 0.5) / size);
    for (let px = 0; px < size; px++) {
      const { lon } = tileToLonLat(z, tx + (px + 0.5) / size, ty);
      fn(py * size + px, lat, lon);
    }
  }
}

/** Terrarium-encoded DEM tile. */
export async function demTile(url: string): Promise<ArrayBuffer> {
  const [z, x, y] = parseZxy(url);
  const size = 256;
  const c = new OffscreenCanvas(size, size);
  const ctx = c.getContext('2d')!;
  const img = ctx.createImageData(size, size);
  forEachPixel(z, x, y, size, (i, lat, lon) => {
    const v = elevation(lat, lon) + 32768;
    img.data[i * 4] = Math.floor(v / 256);
    img.data[i * 4 + 1] = Math.floor(v % 256);
    img.data[i * 4 + 2] = Math.floor((v % 1) * 256);
    img.data[i * 4 + 3] = 255;
  });
  ctx.putImageData(img, 0, 0);
  return canvasToBuffer(c);
}

function hash(x: number, y: number): number {
  const s = Math.sin(x * 127.1 + y * 311.7) * 43758.5453;
  return s - Math.floor(s);
}

/** Muted fake "satellite" imagery: fields, forest on slopes, the river. */
export async function satelliteTile(url: string): Promise<ArrayBuffer> {
  const [z, x, y] = parseZxy(url);
  const size = 256;
  const c = new OffscreenCanvas(size, size);
  const ctx = c.getContext('2d')!;
  const img = ctx.createImageData(size, size);
  forEachPixel(z, x, y, size, (i, lat, lon) => {
    const p = toLocal(lat, lon);
    const e = elevationLocal(p.x, p.y);
    const slope = Math.abs(elevationLocal(p.x + 0.05, p.y) - e) + Math.abs(elevationLocal(p.x, p.y + 0.05) - e);
    const riverY = -3.2 + 0.25 * p.x + 0.6 * Math.sin(p.x / 2.3);
    let r: number, g: number, b: number;
    if (Math.abs(p.y - riverY) < 0.35) {
      [r, g, b] = [28, 44, 52];
    } else {
      const field = hash(Math.floor(p.x / 0.45), Math.floor(p.y / 0.3));
      const forest = slope > 6 || field > 0.82;
      if (forest) [r, g, b] = [38 + field * 10, 52 + field * 12, 34];
      else [r, g, b] = [78 + field * 40, 74 + field * 30, 52 + field * 14];
      const shade = 0.75 + Math.max(-0.2, Math.min(0.2, (elevationLocal(p.x - 0.03, p.y + 0.03) - e) / 20));
      r *= shade;
      g *= shade;
      b *= shade;
    }
    img.data[i * 4] = r;
    img.data[i * 4 + 1] = g;
    img.data[i * 4 + 2] = b;
    img.data[i * 4 + 3] = 255;
  });
  ctx.putImageData(img, 0, 0);
  return canvasToBuffer(c);
}

/** Weather tiles: wind arrows, precipitation blobs, temperature tint; time shifts the field. */
export async function weatherTile(url: string): Promise<ArrayBuffer> {
  const [z, x, y] = parseZxy(url);
  const kind = url.match(/mockwx:\/\/(\w+)/)?.[1] ?? 'wind';
  const timeMatch = decodeURIComponent(url).match(/(\d{4}-\d{2}-\d{2}T\d{2})/);
  const hours = timeMatch ? new Date(timeMatch[1] + ':00:00Z').getTime() / 3.6e6 : 0;
  const size = 256;
  const c = new OffscreenCanvas(size, size);
  const ctx = c.getContext('2d')!;
  if (kind === 'wind') {
    const step = 48;
    for (let py = step / 2; py < size; py += step) {
      for (let px = step / 2; px < size; px += step) {
        const ll = tileToLonLat(z, x + px / size, y + py / size);
        const fromDeg = 290 + 25 * Math.sin(ll.lon * 3 + hours / 6);
        const speed = 6 + 3 * Math.sin(ll.lat * 5 + hours / 4);
        const to = ((fromDeg + 180) * Math.PI) / 180;
        const len = 8 + speed * 1.4;
        const dx = Math.sin(to) * len;
        const dy = -Math.cos(to) * len;
        ctx.strokeStyle = 'rgba(185, 203, 255, 0.85)';
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.moveTo(px - dx / 2, py - dy / 2);
        ctx.lineTo(px + dx / 2, py + dy / 2);
        ctx.stroke();
        const ang = Math.atan2(dy, dx);
        ctx.beginPath();
        ctx.moveTo(px + dx / 2, py + dy / 2);
        ctx.lineTo(px + dx / 2 - 6 * Math.cos(ang - 0.5), py + dy / 2 - 6 * Math.sin(ang - 0.5));
        ctx.lineTo(px + dx / 2 - 6 * Math.cos(ang + 0.5), py + dy / 2 - 6 * Math.sin(ang + 0.5));
        ctx.closePath();
        ctx.fillStyle = 'rgba(185, 203, 255, 0.85)';
        ctx.fill();
      }
    }
  } else {
    const img = ctx.createImageData(size / 4, size / 4);
    const q = size / 4;
    for (let py = 0; py < q; py++) {
      for (let px = 0; px < q; px++) {
        const ll = tileToLonLat(z, x + (px + 0.5) / q, y + (py + 0.5) / q);
        const i = (py * q + px) * 4;
        if (kind === 'precipitation') {
          const v = Math.sin(ll.lon * 22 + hours / 3) * Math.cos(ll.lat * 30 - hours / 5) + 0.3 * Math.sin(ll.lon * 61);
          const a = Math.max(0, v - 0.35) * 260;
          img.data.set([90, 150, 255, Math.min(170, a)], i);
        } else {
          const t = 4 + 3 * Math.sin(ll.lat * 8 + hours / 12) - (elevation(ll.lat, ll.lon) - 450) / 150;
          const f = Math.max(0, Math.min(1, (t + 2) / 12));
          img.data.set([80 + 170 * f, 110 + 40 * (1 - Math.abs(f - 0.5) * 2), 255 - 200 * f, 90], i);
        }
      }
    }
    const small = new OffscreenCanvas(q, q);
    small.getContext('2d')!.putImageData(img, 0, 0);
    ctx.imageSmoothingEnabled = true;
    ctx.drawImage(small, 0, 0, size, size);
  }
  return canvasToBuffer(c);
}

// ---------------- vector data ----------------
function grid(halfKm: number, stepKm: number): { values: number[]; n: number } {
  const n = Math.round((2 * halfKm) / stepKm) + 1;
  const values = new Array<number>(n * n);
  for (let r = 0; r < n; r++) {
    const yk = halfKm - r * stepKm;
    for (let c = 0; c < n; c++) values[r * n + c] = elevationLocal(-halfKm + c * stepKm, yk);
  }
  return { values, n };
}

/** 20 m contour lines as GeoJSON with an `ele` property. */
export function contourLines(): FeatureCollection {
  const half = 14;
  const step = 0.1;
  const { values, n } = grid(half, step);
  const lo = Math.floor(Math.min(...values) / 20) * 20;
  const hi = Math.ceil(Math.max(...values) / 20) * 20;
  const thresholds: number[] = [];
  for (let t = lo; t <= hi; t += 20) thresholds.push(t);
  const gen = contours().size([n, n]).thresholds(thresholds);
  const features: Feature<LineString>[] = [];
  for (const mp of gen(values)) {
    for (const poly of (mp as unknown as MultiPolygon).coordinates) {
      for (const ring of poly) {
        const coords: Position[] = ring
          .filter((p) => p[0] > 0.5 && p[0] < n - 1.5 && p[1] > 0.5 && p[1] < n - 1.5)
          .map(([cx, cy]) => {
            const ll = toLatLon(-half + cx * step, half - cy * step);
            return [ll.lon, ll.lat];
          });
        if (coords.length > 3) features.push({ type: 'Feature', properties: { ele: mp.value }, geometry: { type: 'LineString', coordinates: coords } });
      }
    }
  }
  return { type: 'FeatureCollection', features };
}

function line(points: [number, number][], props: Record<string, unknown>): Feature {
  return { type: 'Feature', properties: props, geometry: { type: 'LineString', coordinates: points.map(([x, y]) => { const ll = toLatLon(x, y); return [ll.lon, ll.lat]; }) } };
}
function polygon(points: [number, number][], props: Record<string, unknown>): Feature {
  const ring = points.map(([x, y]) => { const ll = toLatLon(x, y); return [ll.lon, ll.lat]; });
  ring.push(ring[0]);
  return { type: 'Feature', properties: props, geometry: { type: 'Polygon', coordinates: [ring] } };
}

/** Mock base map in the GeoJSON variant of the base layer (property `layer` = OpenMapTiles source layer). */
export function baseMap(): FeatureCollection {
  const river: [number, number][] = [];
  const bankN: [number, number][] = [];
  const bankS: [number, number][] = [];
  for (let x = -25; x <= 25; x += 0.5) {
    const y = -3.2 + 0.25 * x + 0.6 * Math.sin(x / 2.3);
    river.push([x, y]);
    bankN.push([x, y + 0.28]);
    bankS.push([x, y - 0.28]);
  }
  const features: Feature[] = [
    polygon([...bankN, ...bankS.reverse()], { layer: 'water', class: 'river' }),
    line(river, { layer: 'waterway', class: 'river' }),
    line([[-20, -1.2], [-8, -1.6], [-3, -0.9], [2.5, -1.9], [9, -0.6], [20, -1.4]], { layer: 'transportation', class: 'primary' }),
    line([[-1.6, -1.1], [-0.4, 0.6], [-1.2, 2.4], [-2.4, 3.3], [-3.5, 6.8]], { layer: 'transportation', class: 'minor' }),
    line([[2.6, -1.8], [3.2, 0.8], [5.2, 2.6], [7.5, 3.4]], { layer: 'transportation', class: 'minor' }),
    line([[-9, 4.5], [-4, 4.9], [0.6, 5.6], [6, 6.2], [12, 5.1]], { layer: 'transportation', class: 'minor' }),
    polygon([[-1.2, -1.6], [0.9, -1.8], [1.2, -0.8], [-0.9, -0.6]], { layer: 'landuse', class: 'residential' }),
    polygon([[5.5, 4.8], [7.2, 4.6], [7.6, 6], [5.8, 6.2]], { layer: 'landcover', class: 'wood' }),
    polygon([[-6, 1], [-4.2, 0.6], [-3.9, 2.1], [-5.8, 2.4]], { layer: 'landcover', class: 'wood' }),
  ];
  return { type: 'FeatureCollection', features };
}
