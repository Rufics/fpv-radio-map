# Antenna seeds (rf.antennas)

`seed.json` is loaded into the reference library on first start. Every entry is
`"verified": false`: the admin checks each source and marks it verified in the library editor.

Each entry cites the vendor's own product page, with the spec lines it was taken from in
`source.reference`. Conventions:

- **Circular polarization.** Vendors give CP gain in dBic; it is stored as `gainDbi` as stated.
  Antennas sold in both RHCP and LHCP have one entry (usually RHCP); the note says so. Copy the
  entry and change `polarization` for the other hand.
- **Gain ranges.** Where the vendor gives a range (for example 9.5-10.5 dBic), the lower end is used.
- **Linear antennas.** Vendors say only "linear"; entries use `linear-vertical`, the usual mounting.
- **Band.** Where the vendor names only an ISM band (2.4 GHz, 900 MHz), `bandHz` uses the band
  edges and the note says so.
- **Beam width.** Only the -3 dB width is stored. When the vendor does not name a plane, the note
  says which plane was assumed.
- **Type.** CP omnis (cloverleaf, pagoda, lollipop and similar), crosshair, T and Moxon antennas
  are `other` unless the vendor calls the antenna a dipole or patch.

Not included, because the vendor publishes no gain figure:

- **DJI** O3 and O4 air-unit antennas and the DJI goggles antennas (DJI gives EIRP for the
  system, not antenna gain). Add them from your own measurement (`"kind": "measurement"`).
