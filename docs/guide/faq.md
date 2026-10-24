# FAQ

**Is the result a guarantee?**
No. It is an exact calculation on inputs that carry their own errors: terrain data, the weather forecast and equipment datasheets. The result lists each of them, so you can see what it depends on.

**Why is my real range shorter than predicted?**
Common causes are antenna orientation, a body or frame blocking the antenna, interference, and equipment that performs below its datasheet. A [field report](https://github.com/Rufics/fpv-radio-map/discussions/new?category=show-and-tell) helps improve the model.

**My radio is not in the list.**
Open an [equipment data issue](https://github.com/Rufics/fpv-radio-map/issues/new?template=equipment_data.yml) with a link to the datasheet. Only verified data with a source goes into the library.

**Can I run it offline?**
Yes, on your own server with your own terrain tiles. See [Deployment](../admin/deploy.md).

**How do I see the route in 3D?**
Press **3D** on the map. The terrain rises, the right mouse button turns the map, and two sliders set the tilt and how much heights are exaggerated. **Altitude** opens the profile under the map: the ground, your planned altitude and the safe corridor. Hover the profile to read the height above launch and above the ground at that point; the 3D view marks the same spot. The first time, the profile asks to turn on the altitude corridor for the mission, which needs the `fpv.altitude` module on the server. The 3D relief needs `terrain.pmtiles`; without it the route is drawn over a flat map.

**Can I plan a fixed-wing, not only a multirotor?**
Yes. Pick a drone marked *fixed-wing* under **Drone** in the plan panel. Its library entry carries the flight envelope: stall speed, maximum bank, climb and dive angles, loiter radius. Picking it turns on the `fpv.airframe` check for the mission. The result then has a **Flight checks** card: where the plane cannot climb as steeply as the route asks and comes too close to the terrain, where it reaches a waypoint below or above the planned height, which turns do not fit their legs at the plane's turn radius, and the airspeed margin over the stall in a banked turn. The failsafe check loiters the plane instead of hovering it.

**Do I need a radio link if the autopilot flies the route?**
No. Set **Flight** to *Autonomous* when the route is uploaded before take-off. The links become optional: with **Links to check** set to *None* the server checks no radio link, only that the route stays clear of the ground and, for a fixed-wing, the flight checks above. No coverage map is computed, since it needs a link. If you do evaluate a link, a leg where it is lost is a warning instead of a block, and the **Autonomous: links lost on the way** card lists where the pilot cannot take over or loses video.
