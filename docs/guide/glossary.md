# Glossary

**Link budget.** Transmit power plus antenna gains, minus every loss on the way. The link works while the received power stays above the receiver's sensitivity.

**Link margin.** How far the received power is above the sensitivity, in dB. More margin means the link survives more extra loss, such as a turn of the antenna or a tree line.

**Free-space path loss.** The loss over distance with nothing in the way. It grows with distance and frequency, which is why 5.8 GHz video fades sooner than 900 MHz control.

**Fresnel zone.** The ellipse-shaped space around the straight line between the antennas that carries most of the signal. Terrain inside the first Fresnel zone costs signal even when the drone is still in line of sight.

**Diffraction loss.** Extra loss when terrain blocks the line of sight and the signal bends over a ridge.

**Clutter.** Loss from vegetation and buildings on top of the bare terrain, taken from land-cover data.

**k-factor.** How much the atmosphere bends radio waves. The standard value is 4/3. With a weather forecast the app uses the forecast instead and says so in the result.

**DEM.** Digital elevation model: the terrain heights the calculation uses. Its stated vertical error is shown with the result.

**Sensitivity.** The weakest signal the receiver can still decode, from the radio's datasheet, for a given packet rate or mode.

**RSSI and LQ.** Received signal strength and link quality as your radio reports them in flight. Useful for comparing a prediction with a real flight.

**Safe corridor.** The band of heights the altitude profile and the 3D view draw along the route (module `fpv.altitude`). Its floor is 30 m above the highest ground and obstacles near each point, raised where the link needs more height to keep 60 % of the first Fresnel zone clear. Its ceiling is 120 m above the ground. Both numbers are mission options. Near the pilot the floor rises from the ground, so take-off and landing are not flagged.

**Vertical exaggeration.** The 3D view stretches heights, ×2 by default and up to ×4, so a 50 m ridge on a 5 km route is visible. The current factor is always shown on the map; ×1 is true scale.
