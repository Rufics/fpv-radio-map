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

**How do I climb with the least battery?**
Beside the altitude profile, the **Climb assistant** compares your plan with three ways through the safe corridor: the energy-optimal climb (the least total climb your drone can make at its climb rate), climbing over launch and then flying level, and a steady climb and descent. Each card shows the energy (or, without a drone picked, the total climb), the flight time and the lowest height over the floor against your plan. **Show on profile** draws it as a dashed line; **Preview fix** shows the new waypoint heights, and the points it adds to follow the profile, before you apply them. Set the drone's measured climb rate in the library, or `climbRateMps` in the mission's `fpv.altitude` options, for honest climbs.

**Can I fly the plan before I fly it?**
In 3D, press **▶ Chase**. The camera flies behind the drone along the route at the cruise speed, at the planned heights. The read-out shows the height above the ground and above sea level, the climb or descent the plan asks for there (amber when it is steeper than the drone's climb or descent rate), and whether the plan is inside the safe corridor. The box on the right names the next waypoint and the next place within 2 km where the plan leaves the corridor. The timeline at the bottom is split into legs, coloured by the worst point on each; drag it to jump, and pick ×1, ×4 or ×16. The profile cursor follows the drone. **Exit chase** puts the camera back.

**How does the altitude check look on a phone?**
The map fills the screen under a sheet. With the altitude corridor on, the sheet shows the worst spot of the plan in big numbers: how far it is under (or over) the floor, the height above ground it needs there and the height planned, with one **Preview fix** button for the climb assistant's recommended profile (only when the drone can fly it). Pull the sheet up, or tap its handle, for the altitude profile; **Climb options** opens all the strategies full screen. The **3D** chip at the top switches the map to 3D, and **▶ Chase in 3D** flies the plan.

**The coverage heatmap hides the map. Can I tone it down?**
It already fades on its own: it is weakest while you place or drag points, and turns off during a flight replay, leaving only the line where the margin reaches 0 dB. In **Layers → Coverage** you can also choose **Mini-map instead of the heatmap**, which moves the coverage to a small map in the corner; click it to jump there.

**How do I check the link to one spot without drawing a route?**
After a calculation, turn on **Layers → Coverage → Line of sight on hover** and point at the map. A line from the pilot to the cursor shows the margin, the distance and the ridge in the way. The first value is read from the coverage grid; when the cursor rests, it is replaced by the exact calculation for that point.
**Can I plan a fixed-wing, not only a multirotor?**
Yes. Pick a drone marked *fixed-wing* under **Drone** in the plan panel. Its library entry carries the flight envelope: stall speed, maximum bank, climb and dive angles, loiter radius. Picking it turns on the `fpv.airframe` check for the mission. The result then has a **Flight checks** card: where the plane cannot climb as steeply as the route asks and comes too close to the terrain, where it reaches a waypoint below or above the planned height, which turns do not fit their legs at the plane's turn radius, and the airspeed margin over the stall in a banked turn. The failsafe check loiters the plane instead of hovering it.

**Do I need a radio link if the autopilot flies the route?**
No. Set **Flight** to *Autonomous* when the route is uploaded before take-off. The links become optional: with **Links to check** set to *None* the server checks only that the route stays clear of the ground, and no coverage map is computed, since it needs a link. If you do evaluate a link, a leg where it is lost is a warning instead of a block, and the **Autonomous: links lost on the way** card lists where the pilot cannot take over or loses video.
