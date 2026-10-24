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
