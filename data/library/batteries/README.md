# Battery seeds (rf.batteries)

`seed.json` is loaded into the reference library on first start. Every entry is
`"verified": false`: the admin checks each source and marks it verified in the library editor.

Each entry cites the vendor's own product or specification page, with the spec lines it was
taken from in `source.reference`. Conventions:

- **Capacity.** The vendor's rated (or minimum, where given) capacity.
- **Cell voltage.** The vendor's nominal pack voltage divided by the cell count. Where the vendor
  does not state the cell count (DJI), the note shows how it follows from the nominal and maximum
  charging voltages.
- **usableFraction** is `0.8` everywhere. It is a planning default (a 20% reserve), not a vendor
  figure: set it to the reserve you fly with.
- **temperatureDerating** is left out: no vendor publishes capacity against temperature as
  numbers. Add it from your own measurement.
