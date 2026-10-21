/** Wind arithmetic for display: compass names, the component along a flight track, directions. */
export class Wind {
  private static readonly COMPASS = ['N', 'NNE', 'NE', 'ENE', 'E', 'ESE', 'SE', 'SSE', 'S', 'SSW', 'SW', 'WSW', 'W', 'WNW', 'NW', 'NNW'];

  /** 16-point compass name of a direction in degrees: 250 → "WSW". */
  static compass(deg: number): string {
    const i = Math.round(Wind.normalize(deg) / 22.5) % 16;
    return Wind.COMPASS[i];
  }

  /** Direction the wind blows towards, from the meteorological "from" direction. */
  static towardsDeg(fromDeg: number): number {
    return Wind.normalize(fromDeg + 180);
  }

  /**
   * Component of the wind along a track: positive is a tailwind, negative a headwind.
   * `trackDeg` is the direction the drone flies, degrees clockwise from north.
   */
  static alongTrackMps(speedMps: number, fromDeg: number, trackDeg: number): number {
    const diff = ((Wind.towardsDeg(fromDeg) - trackDeg) * Math.PI) / 180;
    return speedMps * Math.cos(diff);
  }

  /** Direction between `a` and `b` at `t` (0..1), along the shorter arc. */
  static lerpDeg(a: number, b: number, t: number): number {
    const delta = ((b - a + 540) % 360) - 180;
    return Wind.normalize(a + delta * t);
  }

  /** Any angle into 0 ≤ deg < 360. */
  static normalize(deg: number): number {
    return ((deg % 360) + 360) % 360;
  }
}
