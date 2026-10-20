# Radio profile seeds (rf.radio-profiles)

`seed.json` (control links) and `seed-video.json` (DJI video links) are loaded into the reference
library at start, each file once. Every entry is `"verified": false`: the admin checks each source
and marks it verified in the library editor.

Only figures a vendor publishes, or that follow from them, are here.

## DJI digital video (`seed-video.json`)

DJI publishes EIRP, range, latency and channel bandwidth for its video links, but not receiver
sensitivity. The four DJI profiles (O3, O4, O4 Pro, Avata 2) derive the threshold from DJI's own
range claim:

- **Sensitivity** = FCC EIRP limit - free-space loss over the stated FCC range at 5.7875 GHz (the
  middle of 5.725-5.850 GHz) + the goggles antenna gain from DJI's FCC filing (the matching
  `dji-goggles-*` antenna in the antenna library). For the O3: 33 - 127.7 + 3.0 = -91.7 dBm.
  The source kind is `derived` and the note says so.
- **txPowerDbm** = the EIRP limit minus the air antenna gain from DJI's FCC filing, for CE
  (14 dBm EIRP) and FCC.
- Used with those two antennas, a profile reaches DJI's stated range in free space. If DJI's real
  EIRP is below the limit, the real sensitivity is better by the same amount, so the link budget
  still matches DJI's claim.
- DJI's range is for an unobstructed line of sight with no interference, and DJI lowers bandwidth
  and bitrate near the limit, so the threshold is for its narrowest mode. Replace it with your own
  measurement (`"kind": "measurement"`) when you have one.
- **latencyMs** is DJI's stated end-to-end latency for 1080p/100fps with the goggles named.

## Not included

- **Walksnail Avatar**: Caddx publishes EIRP and only "> 4 km" range, with no goggles antenna
  gain, so no threshold follows from its figures.
- **HDZero**: the vendor site gives no sensitivity or range; the -105 dBm VRX figure is only on
  reseller pages.
- **Analog video**: modules publish a sensitivity (TBS Fusion -96 dBm, rapidFIRE better than -96
  dBm, the RX5808 module sheet -90 dBm typical) with no test conditions, and an analog profile
  needs the bandwidth, noise figure and SNR grades for clear, noisy and lost picture, which no
  datasheet gives. Add them from a bench measurement.
