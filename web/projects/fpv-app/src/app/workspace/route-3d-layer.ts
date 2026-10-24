import {
  CustomLayerInterface,
  CustomRenderMethodInput,
  Map as MlMap,
  MercatorCoordinate,
} from 'maplibre-gl';
import { Corridor, CorridorState, FpvAltitudeData } from '../core/fpv-altitude';

/** RGBA, 0–1. */
type Rgba = readonly [number, number, number, number];

const hex = (h: string, a = 1): Rgba => [
  Number.parseInt(h.slice(1, 3), 16) / 255,
  Number.parseInt(h.slice(3, 5), 16) / 255,
  Number.parseInt(h.slice(5, 7), 16) / 255,
  a,
];

/** Colours of the 3D route by where it is in the corridor, matched to the profile drawer. */
export const ROUTE_3D_COLOURS: Record<CorridorState, Rgba> = {
  inside: hex('#35D0BA'),
  takeoff: hex('#F5A524'),
  low: hex('#FF6B5B'),
  ground: hex('#D6453A'),
  high: hex('#5B9CFF'),
};
const SHADOW = hex('#0B0F14', 0.7);
const FLOOR = hex('#F5A524', 0.95);
const CEILING = hex('#8A96A5', 0.75);
const POST = hex('#E8EEF5', 0.75);
const WHITE = hex('#FFFFFF');
const TARGET = hex('#F5A524');
const FRESNEL = hex('#5B9CFF', 0.25);
/** Opacity of the curtain under the line, at the line and at the ground. */
const CURTAIN_TOP = 0.32;
const CURTAIN_FOOT = 0.04;
/** The flight line: a thin core over a wide faint glow, in CSS pixels. */
const LINE_CORE_PX = 3;
const LINE_GLOW_PX = 12;
const LINE_GLOW_ALPHA = 0.2;
/** The shadow track sits this far above the ground, so the terrain does not hide it. */
const SHADOW_LIFT_M = 2;

export interface Route3dOptions {
  exaggeration: number;
  /** The safe corridor as dashed floor and ceiling lines */
  corridor: boolean;
}

export interface Route3dCursorOptions {
  exaggeration: number;
  fresnel: boolean;
  /** Distance along the route of the cursor, m */
  cursorS: number;
}

/** A point in the layer's frame: Web Mercator relative to the origin, and metres above sea level × exaggeration. */
export interface P3 {
  x: number;
  y: number;
  z: number;
}

/** A text label pinned to a point of the scene: a waypoint's heights, or the cursor read-out. */
export interface Route3dLabel {
  at: P3;
  text: string;
  tone: 'point' | 'ok' | 'warn';
}

/** Floats per vertex: position (3) + colour (4) for triangles. */
export const TRI_STRIDE = 7;
/** Floats per vertex for screen-space lines: start (3), end (3), t, side, width px, colour (4). */
export const LINE_STRIDE = 13;
/** Floats per round point: position (3), colour (4), diameter px. */
export const POINT_STRIDE = 8;

const round = (m: number) => Math.round(m).toString();

/** Vertex arrays in the frame of the pilot, and the helpers that place points along the corridor. */
abstract class Route3dGeometry {
  readonly triangles: number[] = [];
  readonly lines: number[] = [];
  readonly points: number[] = [];
  readonly labels: Route3dLabel[] = [];
  readonly origin: MercatorCoordinate;
  protected readonly corridor: Corridor;

  constructor(
    protected readonly data: FpvAltitudeData,
    private readonly exaggeration: number,
  ) {
    const c = data.corridor;
    if (!c || !data.pilot) throw new Error('fpv.altitude data without a corridor');
    this.corridor = new Corridor(c, data.pilot);
    this.origin = MercatorCoordinate.fromLngLat([data.pilot.lon, data.pilot.lat], 0);
  }

  /** Mercator x/y relative to the origin and exaggerated height at a sample. */
  protected at(lat: number, lon: number, mslM: number): P3 {
    const m = MercatorCoordinate.fromLngLat([lon, lat], 0);
    return { x: m.x - this.origin.x, y: m.y - this.origin.y, z: mslM * this.exaggeration };
  }

  protected sample(i: number, h: number[]): P3 {
    return this.at(this.corridor.c.lat[i], this.corridor.c.lon[i], h[i]);
  }

  protected atS(s: number, key: 'groundMslM' | 'plannedMslM'): P3 {
    return this.at(this.corridor.valueAt('lat', s), this.corridor.valueAt('lon', s), this.corridor.valueAt(key, s));
  }

  protected tri(...corners: [P3, Rgba][]): void {
    for (const [p, colour] of corners) this.triangles.push(p.x, p.y, p.z, ...colour);
  }

  protected point(p: P3, colour: Rgba, sizePx: number): void {
    this.points.push(p.x, p.y, p.z, ...colour, sizePx);
  }

  /** Two triangles the vertex shader widens to `width` pixels across the segment's screen direction. */
  protected segment(a: P3, b: P3, width: number, colour: Rgba): void {
    for (const [t, side] of [
      [0, -1],
      [1, -1],
      [1, 1],
      [0, -1],
      [1, 1],
      [0, 1],
    ]) {
      this.lines.push(a.x, a.y, a.z, b.x, b.y, b.z, t, side, width, ...colour);
    }
  }
}

/**
 * The 3D route as vertex arrays: a curtain from the line down to the ground and the line's
 * shadow on the ground, the flight line coloured by where it is in the corridor, posts with a
 * pin and a label at the pilot and the waypoints, and on request the corridor's floor and
 * ceiling as dashed lines. The cursor is a separate, small Route3dCursor, so moving it does not
 * rebuild or re-upload the route.
 */
export class Route3dScene extends Route3dGeometry {
  constructor(
    data: FpvAltitudeData,
    private readonly opts: Route3dOptions,
  ) {
    super(data, opts.exaggeration);
    this.build();
  }

  private build(): void {
    const c = this.corridor.c;
    const n = c.s.length;
    const colour = (i: number) => ROUTE_3D_COLOURS[this.corridor.state(i)];
    for (let i = 0; i + 1 < n; i++) this.curtain(i, colour(i));
    this.polyline(c.groundMslM.map((g) => g + SHADOW_LIFT_M), 3, () => SHADOW);
    if (this.opts.corridor) {
      this.polyline(c.floorMslM, 2, () => FLOOR, true);
      this.polyline(c.ceilingMslM, 1.5, () => CEILING, true);
    }
    this.polyline(c.plannedMslM, LINE_GLOW_PX, (i) => withAlpha(colour(i), LINE_GLOW_ALPHA));
    this.polyline(c.plannedMslM, LINE_CORE_PX, colour);
    const waypoints = this.data.waypoints ?? [];
    this.waypoint(0, 'Pilot', WHITE);
    waypoints.forEach((w, k) => {
      const name = w.label ?? `WP${w.index}`;
      this.waypoint(
        w.s,
        `${name} · ${round(w.aboveLaunchM)} m above launch · ${round(w.aglM)} m AGL`,
        k === waypoints.length - 1 ? TARGET : WHITE,
      );
    });
  }

  /** The curtain from sample i to i + 1: the line's colour at the line, fading out at the ground. */
  private curtain(i: number, colour: Rgba): void {
    const c = this.corridor.c;
    const top = withAlpha(colour, CURTAIN_TOP);
    const foot = withAlpha(colour, CURTAIN_FOOT);
    const a0 = this.sample(i, c.groundMslM);
    const a1 = this.sample(i, c.plannedMslM);
    const b0 = this.sample(i + 1, c.groundMslM);
    const b1 = this.sample(i + 1, c.plannedMslM);
    this.tri([a0, foot], [b0, foot], [b1, top]);
    this.tri([a0, foot], [b1, top], [a1, top]);
  }

  /** A line through every sample; dashed draws every other gap. */
  private polyline(h: number[], width: number, colour: (i: number) => Rgba, dashed = false): void {
    for (let i = 0; i + 1 < h.length; i++)
      if (!dashed || i % 2 === 0) this.segment(this.sample(i, h), this.sample(i + 1, h), width, colour(i));
  }

  /** A post from the ground to the plan, a pin where it meets the ground, a ball on top and a label. */
  private waypoint(s: number, text: string, ball: Rgba): void {
    const foot = this.atS(s, 'groundMslM');
    const top = this.atS(s, 'plannedMslM');
    this.segment(foot, top, 1.5, POST);
    this.point(foot, POST, 7);
    this.point(top, ball, 11);
    this.labels.push({ at: top, text, tone: 'point' });
  }
}

/** The cursor: a dashed drop to the ground, a dot and a read-out, and on request the first Fresnel zone to it. */
export class Route3dCursor extends Route3dGeometry {
  constructor(
    data: FpvAltitudeData,
    private readonly opts: Route3dCursorOptions,
  ) {
    super(data, opts.exaggeration);
    this.cursor(opts.cursorS);
    if (opts.fresnel) this.fresnelZone(opts.cursorS);
  }

  /** The cursor: a dashed drop to the ground, a dot on the line and what the plan is like there. */
  private cursor(s: number): void {
    const foot = this.atS(s, 'groundMslM');
    const top = this.atS(s, 'plannedMslM');
    const dashes = 12;
    for (let k = 0; k < dashes; k++) {
      const t0 = (2 * k) / (2 * dashes);
      const t1 = (2 * k + 1) / (2 * dashes);
      this.segment(lerp(top, foot, t0), lerp(top, foot, t1), 1.5, WHITE);
    }
    this.point(top, WHITE, 12);
    const i = this.corridor.indexAt(s);
    const state = this.corridor.state(i);
    const ground = this.corridor.valueAt('groundMslM', s);
    const agl = this.corridor.valueAt('plannedMslM', s) - ground;
    const need = this.corridor.valueAt('floorMslM', s) - ground;
    const where: Record<CorridorState, string> = {
      inside: 'inside the corridor',
      takeoff: 'take-off',
      low: `needs ${round(need)} m AGL`,
      ground: `below the ground · needs ${round(need)} m AGL`,
      high: 'above the ceiling',
    };
    const warn = state === 'low' || state === 'ground' || state === 'high';
    this.labels.push({
      at: top,
      text: `${(s / 1000).toFixed(1)} km · ${round(agl)} m AGL · ${where[state]}`,
      tone: warn ? 'warn' : 'ok',
    });
  }

  /**
   * The first Fresnel zone from the pilot's antenna to the drone at s: an ellipsoid whose radius
   * at a fraction t of the path is 2·r·√(t(1−t)), with r the radius half way. Built in metres
   * and then exaggerated like everything else, so it matches the drawn heights.
   */
  private fresnelZone(s: number): void {
    const pilot = this.data.pilot;
    if (!pilot) return;
    const r = this.corridor.valueAt('fresnelRadiusM', s);
    if (!Number.isFinite(r) || r <= 0) return;
    const k = this.origin.meterInMercatorCoordinateUnits();
    const e = this.opts.exaggeration;
    const b = this.atS(s, 'plannedMslM');
    // In metres from the antenna; y is southward like Web Mercator, which keeps the frame right-handed enough for drawing.
    const d = [b.x / k, b.y / k, b.z / e - pilot.antennaMslM];
    const len = Math.hypot(d[0], d[1], d[2]);
    if (len < 1) return;
    const u = d.map((v) => v / len);
    const v = normalise(cross(u, Math.abs(u[2]) < 0.9 ? [0, 0, 1] : [1, 0, 0]));
    const w = cross(u, v);
    const rings = 24;
    const sides = 16;
    const point = (i: number, j: number): [P3, Rgba] => {
      const t = i / rings;
      const rad = 2 * r * Math.sqrt(t * (1 - t));
      const a = (j / sides) * 2 * Math.PI;
      const q = [0, 1, 2].map(
        (x) => u[x] * t * len + (v[x] * Math.cos(a) + w[x] * Math.sin(a)) * rad,
      );
      return [{ x: q[0] * k, y: q[1] * k, z: (pilot.antennaMslM + q[2]) * e }, FRESNEL];
    };
    for (let i = 0; i < rings; i++) {
      for (let j = 0; j < sides; j++) {
        this.tri(point(i, j), point(i + 1, j), point(i + 1, j + 1));
        this.tri(point(i, j), point(i + 1, j + 1), point(i, j + 1));
      }
    }
  }
}

function withAlpha(c: Rgba, a: number): Rgba {
  return [c[0], c[1], c[2], a];
}

function lerp(a: P3, b: P3, t: number): P3 {
  return { x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t, z: a.z + (b.z - a.z) * t };
}

function cross(a: number[], b: number[]): number[] {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}

function normalise(a: number[]): number[] {
  const l = Math.hypot(a[0], a[1], a[2]);
  return a.map((x) => x / l);
}

const TRI_VS = `#version 300 es
uniform mat4 u_matrix;
in vec3 a_pos;
in vec4 a_color;
out vec4 v_color;
void main() {
  gl_Position = u_matrix * vec4(a_pos, 1.0);
  v_color = a_color;
}`;

const LINE_VS = `#version 300 es
uniform mat4 u_matrix;
uniform vec2 u_viewport;
in vec3 a_start;
in vec3 a_end;
in vec3 a_tsw;
in vec4 a_color;
out vec4 v_color;
void main() {
  vec4 s = u_matrix * vec4(a_start, 1.0);
  vec4 e = u_matrix * vec4(a_end, 1.0);
  vec2 dir = e.xy / e.w - s.xy / s.w;
  dir = length(dir * u_viewport) > 1e-6 ? normalize(dir * u_viewport) : vec2(1.0, 0.0);
  vec2 normal = vec2(-dir.y, dir.x);
  vec4 p = mix(s, e, a_tsw.x);
  p.xy += normal * a_tsw.y * a_tsw.z / u_viewport * p.w;
  gl_Position = p;
  v_color = a_color;
}`;

const FS = `#version 300 es
precision mediump float;
in vec4 v_color;
out vec4 fragColor;
void main() {
  fragColor = vec4(v_color.rgb * v_color.a, v_color.a);
}`;

const POINT_VS = `#version 300 es
uniform mat4 u_matrix;
uniform float u_ratio;
in vec3 a_pos;
in vec4 a_color;
in float a_size;
out vec4 v_color;
void main() {
  gl_Position = u_matrix * vec4(a_pos, 1.0);
  gl_PointSize = a_size * u_ratio;
  v_color = a_color;
}`;

/** A round dot with a dark rim, so a white ball reads over light ground too. */
const POINT_FS = `#version 300 es
precision mediump float;
in vec4 v_color;
out vec4 fragColor;
void main() {
  float r = length(gl_PointCoord - 0.5);
  if (r > 0.5) discard;
  vec3 c = r > 0.36 ? vec3(0.043, 0.059, 0.078) : v_color.rgb;
  fragColor = vec4(c * v_color.a, v_color.a);
}`;

interface Program {
  program: WebGLProgram;
  attribs: Record<string, number>;
  matrix: WebGLUniformLocation | null;
  viewport: WebGLUniformLocation | null;
  ratio: WebGLUniformLocation | null;
}

/** One drawn part of the layer (the route, or the cursor) with its own buffers, uploaded when it changes. */
class Part {
  geometry: Route3dGeometry | null = null;
  uploaded = true;
  tri?: WebGLBuffer;
  line?: WebGLBuffer;
  point?: WebGLBuffer;

  set(geometry: Route3dGeometry | null): void {
    this.geometry = geometry;
    this.uploaded = false;
  }

  create(gl: WebGL2RenderingContext): void {
    this.tri = gl.createBuffer() ?? undefined;
    this.line = gl.createBuffer() ?? undefined;
    this.point = gl.createBuffer() ?? undefined;
    this.uploaded = false;
  }

  delete(gl: WebGL2RenderingContext): void {
    for (const b of [this.tri, this.line, this.point]) if (b) gl.deleteBuffer(b);
    this.tri = this.line = this.point = undefined;
  }
}

/**
 * MapLibre custom layer that draws a Route3dScene and a Route3dCursor in 3D, over the terrain.
 * Each has its own buffers, so moving the cursor uploads only the cursor. Labels are DOM
 * elements in a host the component gives it, moved to their projected points on every frame.
 */
export class Route3dLayer implements CustomLayerInterface {
  readonly id = 'fpv-route-3d';
  readonly type = 'custom' as const;
  readonly renderingMode = '3d' as const;

  private map?: MlMap;
  private tri?: Program;
  private line?: Program;
  private dot?: Program;
  private readonly route = new Part();
  private readonly cursor = new Part();
  private labelHost: HTMLElement | null = null;
  private labelEls: HTMLElement[] = [];

  /** The route, which is drawn first; the cursor is drawn over it. */
  get scene(): Route3dScene | null {
    return this.route.geometry as Route3dScene | null;
  }

  setScene(scene: Route3dScene | null): void {
    this.route.set(scene);
    this.changed();
  }

  setCursor(cursor: Route3dCursor | null): void {
    this.cursor.set(cursor);
    this.changed();
  }

  private changed(): void {
    this.syncLabels();
    this.map?.triggerRepaint();
  }

  /** Where the labels go: an element over the map canvas, or null to drop them. */
  setLabelHost(host: HTMLElement | null): void {
    for (const el of this.labelEls) el.remove();
    this.labelEls = [];
    this.labelHost = host;
    this.syncLabels();
  }

  /** The route's labels, then the cursor's; a cursor needs a route to be drawn. */
  private labels(): Route3dLabel[] {
    const route = this.route.geometry;
    return route ? [...route.labels, ...(this.cursor.geometry?.labels ?? [])] : [];
  }

  /** One element per label, reused across updates so the cursor read-out does not flicker. */
  private syncLabels(): void {
    const labels = this.labels();
    const host = this.labelHost;
    if (!host) return;
    while (this.labelEls.length > labels.length) this.labelEls.pop()?.remove();
    while (this.labelEls.length < labels.length) {
      const el = document.createElement('div');
      el.hidden = true;
      host.appendChild(el);
      this.labelEls.push(el);
    }
    labels.forEach((l, i) => {
      const el = this.labelEls[i];
      el.className = `fpv-3d-label is-${l.tone}`;
      el.textContent = l.text;
    });
  }

  /** Moves each label to its point on screen; hidden behind the camera or far off screen. */
  private placeLabels(matrix: Float32Array, width: number, height: number): void {
    this.labels().forEach((l, i) => {
      const el = this.labelEls[i];
      if (!el) return;
      const { x, y, z } = l.at;
      const cx = matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12];
      const cy = matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13];
      const w = matrix[3] * x + matrix[7] * y + matrix[11] * z + matrix[15];
      const px = ((cx / w + 1) / 2) * width;
      const py = ((1 - cy / w) / 2) * height;
      el.hidden = w <= 0 || px < -100 || px > width + 100 || py < -50 || py > height + 50;
      if (!el.hidden) el.style.transform = `translate(${px.toFixed(1)}px, ${py.toFixed(1)}px)`;
    });
  }

  onAdd(map: MlMap, gl: WebGL2RenderingContext): void {
    this.map = map;
    this.tri = link(gl, TRI_VS, FS, ['a_pos', 'a_color']);
    this.line = link(gl, LINE_VS, FS, ['a_start', 'a_end', 'a_tsw', 'a_color']);
    this.dot = link(gl, POINT_VS, POINT_FS, ['a_pos', 'a_color', 'a_size']);
    this.route.create(gl);
    this.cursor.create(gl);
  }

  onRemove(_map: MlMap, gl: WebGL2RenderingContext): void {
    for (const p of [this.tri, this.line, this.dot]) if (p) gl.deleteProgram(p.program);
    this.route.delete(gl);
    this.cursor.delete(gl);
    for (const el of this.labelEls) el.hidden = true;
    this.map = undefined;
  }

  render(gl: WebGL2RenderingContext, args: CustomRenderMethodInput): void {
    const scene = this.route.geometry;
    const { tri, line, dot } = this;
    if (!scene || !this.map || !tri || !line || !dot) return;
    const matrix = layerMatrix(
      args.modelViewProjectionMatrix,
      scene.origin,
      512 * 2 ** this.map.getZoom(),
    );
    // Line widths are in CSS pixels; MapLibre sizes its buffer by its own pixel ratio.
    const ratio = this.map.getPixelRatio();
    const width = gl.drawingBufferWidth / ratio;
    const height = gl.drawingBufferHeight / ratio;
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.depthMask(false);
    for (const part of [this.route, this.cursor]) this.draw(gl, part, matrix, ratio, width, height);
    for (const p of [tri, line, dot])
      for (const a of Object.values(p.attribs)) if (a >= 0) gl.disableVertexAttribArray(a);
    this.placeLabels(matrix, width, height);
  }

  private draw(
    gl: WebGL2RenderingContext,
    part: Part,
    matrix: Float32Array,
    ratio: number,
    width: number,
    height: number,
  ): void {
    const g = part.geometry;
    const { tri, line, dot } = this;
    if (!g || !tri || !line || !dot || !part.tri || !part.line || !part.point) return;
    if (!part.uploaded) {
      gl.bindBuffer(gl.ARRAY_BUFFER, part.tri);
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(g.triangles), gl.STATIC_DRAW);
      gl.bindBuffer(gl.ARRAY_BUFFER, part.line);
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(g.lines), gl.STATIC_DRAW);
      gl.bindBuffer(gl.ARRAY_BUFFER, part.point);
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(g.points), gl.STATIC_DRAW);
      part.uploaded = true;
    }

    gl.useProgram(tri.program);
    gl.uniformMatrix4fv(tri.matrix, false, matrix);
    gl.bindBuffer(gl.ARRAY_BUFFER, part.tri);
    attrib(gl, tri.attribs['a_pos'], 3, TRI_STRIDE, 0);
    attrib(gl, tri.attribs['a_color'], 4, TRI_STRIDE, 3);
    gl.drawArrays(gl.TRIANGLES, 0, g.triangles.length / TRI_STRIDE);

    gl.useProgram(line.program);
    gl.uniformMatrix4fv(line.matrix, false, matrix);
    gl.uniform2f(line.viewport, width, height);
    gl.bindBuffer(gl.ARRAY_BUFFER, part.line);
    attrib(gl, line.attribs['a_start'], 3, LINE_STRIDE, 0);
    attrib(gl, line.attribs['a_end'], 3, LINE_STRIDE, 3);
    attrib(gl, line.attribs['a_tsw'], 3, LINE_STRIDE, 6);
    attrib(gl, line.attribs['a_color'], 4, LINE_STRIDE, 9);
    gl.drawArrays(gl.TRIANGLES, 0, g.lines.length / LINE_STRIDE);

    gl.useProgram(dot.program);
    gl.uniformMatrix4fv(dot.matrix, false, matrix);
    gl.uniform1f(dot.ratio, ratio);
    gl.bindBuffer(gl.ARRAY_BUFFER, part.point);
    attrib(gl, dot.attribs['a_pos'], 3, POINT_STRIDE, 0);
    attrib(gl, dot.attribs['a_color'], 4, POINT_STRIDE, 3);
    attrib(gl, dot.attribs['a_size'], 1, POINT_STRIDE, 7);
    gl.drawArrays(gl.POINTS, 0, g.points.length / POINT_STRIDE);
  }
}

/**
 * MapLibre's view-projection takes world pixels for x/y and metres for z. The scene keeps x/y as
 * small Mercator offsets from its origin (so float32 keeps them exact), and this folds the origin
 * and the world size back in, in double precision.
 */
export function layerMatrix(
  viewProj: ArrayLike<number>,
  origin: { x: number; y: number },
  worldSize: number,
): Float32Array {
  const m = Array.from(viewProj);
  const out = new Float32Array(16);
  for (let r = 0; r < 4; r++) {
    out[r] = m[r] * worldSize;
    out[4 + r] = m[4 + r] * worldSize;
    out[8 + r] = m[8 + r];
    out[12 + r] = m[r] * origin.x * worldSize + m[4 + r] * origin.y * worldSize + m[12 + r];
  }
  return out;
}

function attrib(
  gl: WebGL2RenderingContext,
  loc: number,
  size: number,
  stride: number,
  offset: number,
): void {
  if (loc < 0) return;
  gl.enableVertexAttribArray(loc);
  gl.vertexAttribPointer(loc, size, gl.FLOAT, false, stride * 4, offset * 4);
}

function link(gl: WebGL2RenderingContext, vs: string, fs: string, names: string[]): Program {
  const program = gl.createProgram();
  for (const [type, src] of [
    [gl.VERTEX_SHADER, vs],
    [gl.FRAGMENT_SHADER, fs],
  ] as const) {
    const shader = gl.createShader(type);
    if (!shader) throw new Error('WebGL shader could not be created');
    gl.shaderSource(shader, src);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS))
      throw new Error(gl.getShaderInfoLog(shader) ?? 'shader');
    gl.attachShader(program, shader);
  }
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS))
    throw new Error(gl.getProgramInfoLog(program) ?? 'program');
  const attribs: Record<string, number> = {};
  for (const n of names) attribs[n] = gl.getAttribLocation(program, n);
  return {
    program,
    attribs,
    matrix: gl.getUniformLocation(program, 'u_matrix'),
    viewport: gl.getUniformLocation(program, 'u_viewport'),
    ratio: gl.getUniformLocation(program, 'u_ratio'),
  };
}
