# Drone seeds (rf.drones)

`seed.json` is loaded into the reference library on first start. Every entry is
`"verified": false`: the admin checks each source and marks it verified in the library editor.

No vendor publishes the electrical power at cruise airspeed or the climb efficiency, so these
profiles are derived (`"kind": "derived"`) from figures DJI does publish, and the note on each
entry shows the arithmetic:

- **Cruise power** is the pack energy divided by the time the vendor flew at a stated constant
  speed in still air at sea level, from full charge to a forced landing. It is an average over the
  whole pack, so it includes the higher draw near the end of discharge. Where the vendor gives
  both a flight-time and a flight-distance figure, the faster of the two speeds is used, as it is
  closer to how an FPV drone cruises; the note gives the other figure.
- **Mass** is the vendor's takeoff weight with the stock battery.
- **Climb** uses the potential-energy method with an efficiency of 0.3. This is a planning value,
  not a vendor figure; it is set low so climbs cost more rather than less. For these drones a
  100 m climb at 0.3 costs 1 to 2% of the pack, so the choice moves little.
- `powerDensityExponent` is left out (0), so power is not corrected for air density at altitude.

Not included:

- **DJI Avata** (first generation): DJI publishes hover time and flight distance but not the
  speed for either, so a cruise power cannot be derived.

Replace a derived profile with your own flight-log measurement (`"kind": "measurement"`) when
you have one.

## Fixed-wing profiles

A drone is a multirotor unless its profile says `"airframe": "fixed-wing"`. A fixed-wing profile
also needs `fixedWing`, the flight envelope as the flight controller is set, because the plane
flies the route within it:

- `stallSpeedMps`: level-flight stall airspeed at the profile's mass. In a turn at the bank
  angle the stall rises to Vs / sqrt(cos bank), and `fpv.airframe` compares the cruise airspeed
  with that.
- `maxBankDeg`: the bank the autopilot turns with (INAV `nav_fw_bank_angle`, ArduPilot
  `ROLL_LIMIT_DEG`). The turn radius is V² / (g tan bank), with the wind added to V over the ground.
- `maxClimbAngleDeg` and `maxDiveAngleDeg`: the steepest climb and descent (INAV
  `nav_fw_climb_angle` and `nav_fw_dive_angle`, ArduPilot `TECS_PITCH_MAX` and `TECS_PITCH_MIN`).
- `loiterRadiusM` (optional): the circle the plane holds position on (INAV `nav_fw_loiter_radius`,
  ArduPilot `WP_LOITER_RAD`); without it the tightest turn at cruise is used.

The seeds hold no fixed-wing profile yet: copy these values from your flight controller and add a
`source` for the stall speed, which is the one figure the controller does not hold.
