# Radio profile seeds (rf.radio-profiles)

`seed.json` is loaded into the reference library on first start. Every entry is
`"verified": false`: the admin checks each source and marks it verified in the library editor.

Only figures a vendor publishes are here. Not included, because no vendor figure exists to
cite:

- **Digital video** (DJI O3/O4, Walksnail, HDZero): the vendors publish EIRP and bandwidth
  but not receiver sensitivity. Add a profile from your own measurement (`"kind": "measurement"`).
- **Analog video**: the RX5808 module sheet gives -90 dBm typical (-88 dBm minimum) with no
  test conditions, and analog profiles need SNR grades for clear, noisy and lost picture,
  which no datasheet gives. Add them from a bench measurement.
