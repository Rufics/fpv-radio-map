/** What the pilot is doing, as far as the coverage heatmap cares. */
export type CoverageActivity = 'analyse' | 'edit' | 'drag' | 'flight' | 'mini-map';

/** The state the heatmap's intensity follows. */
export interface CoverageActivityState {
  /** A flight is replayed or compared with the plan: only the 0 dB edge stays. */
  flight: boolean;
  /** The coverage is shown in the mini-map instead of on the map. */
  miniMap: boolean;
  /** A point is being dragged. */
  dragging: boolean;
  /** Points are being placed or the route is open for editing. */
  editing: boolean;
}

/**
 * How strongly the coverage heatmap is drawn for what the pilot is doing: full while the result
 * is read, faint while the route is drawn or edited, almost gone while a point is dragged, and
 * off during a flight replay or while the mini-map shows it. The 0 dB edge is a separate layer and
 * stays in every case.
 */
export class CoverageIntensity {
  static readonly OPACITY: Readonly<Record<CoverageActivity, number>> = {
    analyse: 0.45,
    edit: 0.12,
    drag: 0.06,
    flight: 0,
    'mini-map': 0,
  };

  private static readonly NOTES: Readonly<Record<CoverageActivity, string | null>> = {
    analyse: null,
    edit: 'Faded while you edit the route',
    drag: 'Faded while you move a point',
    flight: null,
    'mini-map': 'Shown in the mini-map',
  };

  static activity(s: CoverageActivityState): CoverageActivity {
    if (s.flight) return 'flight';
    if (s.miniMap) return 'mini-map';
    if (s.dragging) return 'drag';
    return s.editing ? 'edit' : 'analyse';
  }

  static opacity(activity: CoverageActivity): number {
    return CoverageIntensity.OPACITY[activity];
  }

  /** A line for the legend when the heatmap is not at full strength, or null. */
  static note(activity: CoverageActivity): string | null {
    return CoverageIntensity.NOTES[activity];
  }
}
