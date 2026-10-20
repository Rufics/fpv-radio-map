# Drone seeds (rf.drones)

There is no `seed.json` here on purpose. A drone profile needs the electrical power at cruise
airspeed and the climb efficiency (or a measured climb power). No vendor publishes the climb
figures, and the cruise power could only be inferred from a hover or flight-time claim made
under the vendor's own test conditions. Add drone profiles from your own flight logs
(`"kind": "measurement"`).
