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

/** Colours of the 3D route, matched to the profile drawer. */
export const ROUTE_3D_COLOURS: Record<CorridorState, Rgba> = {
  inside: hex('#35D0BA'),
  low: hex('#FFB547'),
  ground: hex('#FF6B5B'),
  high: hex('#B98CFF'),
  takeoff: hex('#8A96A5'),
};
const CURTAIN = hex('#B9CBFF', 0.16);
const BAND = hex('#35D0BA', 0.1);
const FLOOR = hex('#FFB547', 0.9);
const CEILING = hex('#6EA8FE', 0.9);
const POST = hex('#E8EEF5', 0.85);
const CURSOR = hex('#FFFFFF', 1);
const FRESNEL = hex('#6EA8FE', 0.3);

export interface Route3dOptions {
  exaggeration: number;
  corridor: boolean;
  fresnel: boolean;
  /** Distance along the route of the cursor, m */
  cursorS: number | null;
}

/** A point in the layer's frame: Web Mercator relative to the origin, and metres above sea level × exaggeration. */
interface P3 {
  x: number;
  y: number;
  z: number;
}

/** Floats per vertex: position (3) + colour (4) for triangles. */
export const TRI_STRIDE = 7;
/** Floats per vertex for screen-space lines: start (3), end (3), t, side, width px, colour (4). */
export const LINE_STRIDE = 13;

/**
 * The 3D route as vertex arrays: the curtain from the ground to the plan, the safe corridor
 * between floor and ceiling, the flight line coloured by where it is in the corridor, posts at
 * the pilot and the waypoints, the cursor, and the first Fresnel zone to the cursor.
 */
export class Route3dScene {
  readonly triangles: number[] = [];
  readonly lines: number[] = [];
  readonly origin: MercatorCoordinate;
  private readonly corridor: Corridor;

  constructor(
    private readonly data: FpvAltitudeData,
    private readonly opts: Route3dOptions,
  ) {
    const c = data.corridor;
    if (!c || !data.pilot) throw new Error('fpv.altitude data without a corridor');
    this.corridor = new Corridor(c, data.pilot);
    this.origin = MercatorCoordinate.fromLngLat([data.pilot.lon, data.pilot.lat], 0);
    this.build();
  }

  private build(): void {
    const c = this.corridor.c;
    const n = c.s.length;
    for (let i = 0; i + 1 < n; i++) {
      this.wall(i, c.groundMslM, c.plannedMslM, CURTAIN);
      if (this.opts.corridor) this.wall(i, c.floorMslM, c.ceilingMslM, BAND);
    }
    if (this.opts.corridor) {
      this.polyline(c.floorMslM, 2, () => FLOOR);
      this.polyline(c.ceilingMslM, 2, () => CEILING);
    }
    this.polyline(c.plannedMslM, 4, (i) => ROUTE_3D_COLOURS[this.corridor.state(i)]);
    this.post(0, 2, POST);
    for (const w of this.data.waypoints ?? []) this.post(w.s, 2, POST);
    const s = this.opts.cursorS;
    if (s !== null) {
      this.post(s, 3, CURSOR);
      if (this.opts.fresnel) this.fresnelZone(s);
    }
  }

  /** Mercator x/y relative to the origin and exaggerated height at a sample. */
  private at(lat: number, lon: number, mslM: number): P3 {
    const m = MercatorCoordinate.fromLngLat([lon, lat], 0);
    return { x: m.x - this.origin.x, y: m.y - this.origin.y, z: mslM * this.opts.exaggeration };
  }

  private sample(i: number, h: number[]): P3 {
    return this.at(this.corridor.c.lat[i], this.corridor.c.lon[i], h[i]);
  }

  /** A vertical quad between two heights from sample i to i + 1. */
  private wall(i: number, bottom: number[], top: number[], colour: Rgba): void {
    const a0 = this.sample(i, bottom);
    const a1 = this.sample(i, top);
    const b0 = this.sample(i + 1, bottom);
    const b1 = this.sample(i + 1, top);
    this.tri(a0, b0, b1, colour);
    this.tri(a0, b1, a1, colour);
  }

  private polyline(h: number[], width: number, colour: (i: number) => Rgba): void {
    for (let i = 0; i + 1 < h.length; i++)
      this.segment(this.sample(i, h), this.sample(i + 1, h), width, colour(i));
  }

  /** A vertical post from the ground to the plan at distance s. */
  private post(s: number, width: number, colour: Rgba): void {
    const lat = this.corridor.valueAt('lat', s);
    const lon = this.corridor.valueAt('lon', s);
    this.segment(
      this.at(lat, lon, this.corridor.valueAt('groundMslM', s)),
      this.at(lat, lon, this.corridor.valueAt('plannedMslM', s)),
      width,
      colour,
    );
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
    const b = this.at(
      this.corridor.valueAt('lat', s),
      this.corridor.valueAt('lon', s),
      this.corridor.valueAt('plannedMslM', s),
    );
    // In metres from the antenna; y is southward like Web Mercator, which keeps the frame right-handed enough for drawing.
    const d = [b.x / k, b.y / k, b.z / e - pilot.antennaMslM];
    const len = Math.hypot(d[0], d[1], d[2]);
    if (len < 1) return;
    const u = d.map((v) => v / len);
    const v = normalise(cross(u, Math.abs(u[2]) < 0.9 ? [0, 0, 1] : [1, 0, 0]));
    const w = cross(u, v);
    const rings = 24;
    const sides = 16;
    const point = (i: number, j: number): P3 => {
      const t = i / rings;
      const rad = 2 * r * Math.sqrt(t * (1 - t));
      const a = (j / sides) * 2 * Math.PI;
      const q = [0, 1, 2].map(
        (x) => u[x] * t * len + (v[x] * Math.cos(a) + w[x] * Math.sin(a)) * rad,
      );
      return { x: q[0] * k, y: q[1] * k, z: (pilot.antennaMslM + q[2]) * e };
    };
    for (let i = 0; i < rings; i++) {
      for (let j = 0; j < sides; j++) {
        this.tri(point(i, j), point(i + 1, j), point(i + 1, j + 1), FRESNEL);
        this.tri(point(i, j), point(i + 1, j + 1), point(i, j + 1), FRESNEL);
      }
    }
  }

  private tri(a: P3, b: P3, c: P3, colour: Rgba): void {
    for (const p of [a, b, c]) this.triangles.push(p.x, p.y, p.z, ...colour);
  }

  /** Two triangles the vertex shader widens to `width` pixels across the segment's screen direction. */
  private segment(a: P3, b: P3, width: number, colour: Rgba): void {
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

interface Program {
  program: WebGLProgram;
  attribs: Record<string, number>;
  matrix: WebGLUniformLocation | null;
  viewport: WebGLUniformLocation | null;
}

/** MapLibre custom layer that draws a Route3dScene in 3D, over the terrain. */
export class Route3dLayer implements CustomLayerInterface {
  readonly id = 'fpv-route-3d';
  readonly type = 'custom' as const;
  readonly renderingMode = '3d' as const;

  private map?: MlMap;
  private tri?: Program;
  private line?: Program;
  private triBuffer?: WebGLBuffer;
  private lineBuffer?: WebGLBuffer;
  private scene: Route3dScene | null = null;
  private uploaded = true;

  setScene(scene: Route3dScene | null): void {
    this.scene = scene;
    this.uploaded = false;
    this.map?.triggerRepaint();
  }

  onAdd(map: MlMap, gl: WebGL2RenderingContext): void {
    this.map = map;
    this.tri = link(gl, TRI_VS, ['a_pos', 'a_color']);
    this.line = link(gl, LINE_VS, ['a_start', 'a_end', 'a_tsw', 'a_color']);
    this.triBuffer = gl.createBuffer() ?? undefined;
    this.lineBuffer = gl.createBuffer() ?? undefined;
    this.uploaded = false;
  }

  onRemove(_map: MlMap, gl: WebGL2RenderingContext): void {
    if (this.tri) gl.deleteProgram(this.tri.program);
    if (this.line) gl.deleteProgram(this.line.program);
    if (this.triBuffer) gl.deleteBuffer(this.triBuffer);
    if (this.lineBuffer) gl.deleteBuffer(this.lineBuffer);
    this.map = undefined;
  }

  render(gl: WebGL2RenderingContext, args: CustomRenderMethodInput): void {
    const scene = this.scene;
    if (!scene || !this.map || !this.tri || !this.line || !this.triBuffer || !this.lineBuffer)
      return;
    if (!this.uploaded) {
      gl.bindBuffer(gl.ARRAY_BUFFER, this.triBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(scene.triangles), gl.STATIC_DRAW);
      gl.bindBuffer(gl.ARRAY_BUFFER, this.lineBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(scene.lines), gl.STATIC_DRAW);
      this.uploaded = true;
    }
    const matrix = layerMatrix(
      args.modelViewProjectionMatrix,
      scene.origin,
      512 * 2 ** this.map.getZoom(),
    );
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.depthMask(false);

    gl.useProgram(this.tri.program);
    gl.uniformMatrix4fv(this.tri.matrix, false, matrix);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.triBuffer);
    attrib(gl, this.tri.attribs['a_pos'], 3, TRI_STRIDE, 0);
    attrib(gl, this.tri.attribs['a_color'], 4, TRI_STRIDE, 3);
    gl.drawArrays(gl.TRIANGLES, 0, scene.triangles.length / TRI_STRIDE);

    gl.useProgram(this.line.program);
    gl.uniformMatrix4fv(this.line.matrix, false, matrix);
    // Line widths are in CSS pixels; MapLibre sizes its buffer by its own pixel ratio.
    const ratio = this.map.getPixelRatio();
    gl.uniform2f(this.line.viewport, gl.drawingBufferWidth / ratio, gl.drawingBufferHeight / ratio);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.lineBuffer);
    attrib(gl, this.line.attribs['a_start'], 3, LINE_STRIDE, 0);
    attrib(gl, this.line.attribs['a_end'], 3, LINE_STRIDE, 3);
    attrib(gl, this.line.attribs['a_tsw'], 3, LINE_STRIDE, 6);
    attrib(gl, this.line.attribs['a_color'], 4, LINE_STRIDE, 9);
    gl.drawArrays(gl.TRIANGLES, 0, scene.lines.length / LINE_STRIDE);
    for (const a of [...Object.values(this.tri.attribs), ...Object.values(this.line.attribs)])
      if (a >= 0) gl.disableVertexAttribArray(a);
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

function link(gl: WebGL2RenderingContext, vs: string, names: string[]): Program {
  const program = gl.createProgram();
  for (const [type, src] of [
    [gl.VERTEX_SHADER, vs],
    [gl.FRAGMENT_SHADER, FS],
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
  };
}
