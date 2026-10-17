import { Diffraction, LinkEnd, LinkResult, Profile, ProfilePoint } from '../models/api.models';

/** A link drawn left = ground (pilot), right = air (drone), whatever end transmits. */
export interface GroundToAirView {
  profile?: Profile;
  diffraction?: Diffraction;
  ground?: LinkEnd;
  air?: LinkEnd;
  /** true when the server ordered the profile from the drone (video link) and it was flipped */
  flipped: boolean;
}

/**
 * The contract orders `profile` (and diffraction distances) from the transmitter: drone → pilot for
 * video, pilot → drone for control. Charts always show the pilot on the left, so flip when the
 * transmitter is the air end. Heights come from `ends`, never from guesses.
 */
export function groundToAir(link: LinkResult | null | undefined): GroundToAirView {
  const ground = link?.ends?.ground;
  const air = link?.ends?.air;
  const flipped = link?.ends?.transmitter === 'air';
  const profile = link?.profile;
  if (!flipped || !profile?.points?.length) return { profile, diffraction: link?.diffraction, ground, air, flipped: false };
  const L = profile.lengthM ?? profile.points[profile.points.length - 1].distanceM;
  const flipPoint = (p: ProfilePoint | undefined): ProfilePoint | undefined => (p ? { ...p, distanceM: L - p.distanceM } : p);
  const points = [...profile.points].reverse().map((p) => flipPoint(p)!);
  const d = link?.diffraction;
  const diffraction: Diffraction | undefined = d
    ? { ...d, d1M: d.d2M, d2M: d.d1M, dominant: flipPoint(d.dominant), minClearancePoint: flipPoint(d.minClearancePoint) }
    : d;
  return { profile: { ...profile, points }, diffraction, ground, air, flipped: true };
}
