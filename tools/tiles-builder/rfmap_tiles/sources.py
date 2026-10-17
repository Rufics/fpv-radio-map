"""Data sources, default URLs and the attribution text each output must carry.

Attribution strings are what the map must display. Licence notes are what we
know about each source; the operator is responsible for checking the current
licence text before publishing (see docs/admin/tiles-builder.md).
"""

# --- OpenStreetMap / OpenMapTiles (basemap) ---------------------------------
GEOFABRIK_BASE = "https://download.geofabrik.de"
GEOFABRIK_INDEX = GEOFABRIK_BASE + "/index-v1-nogeom.json"
# Defaults Planetiler itself uses for its OpenMapTiles profile:
WATER_POLYGONS_URL = "https://osmdata.openstreetmap.de/download/water-polygons-split-3857.zip"
NATURAL_EARTH_URL = "https://naciscdn.org/naturalearth/packages/natural_earth_vector.sqlite.zip"
# Pinned to the release Planetiler 0.10.2 uses by default:
LAKE_CENTERLINES_URL = "https://github.com/acalcutt/osm-lakelines/releases/download/v12/lake_centerline.shp.zip"

OSM = {
    "source": "OpenStreetMap data, OpenMapTiles schema (built with Planetiler); "
              "Natural Earth (low zooms); OSM water polygons (osmdata.openstreetmap.de)",
    "licence": "OSM data: Open Database License (ODbL) 1.0. OpenMapTiles schema: CC BY 4.0 "
               "(attribution required). Natural Earth: public domain.",
    "attribution": "© OpenMapTiles © OpenStreetMap contributors",
    "attribution_html": '<a href="https://openmaptiles.org/" target="_blank">© OpenMapTiles</a> '
                        '<a href="https://www.openstreetmap.org/copyright" target="_blank">© OpenStreetMap contributors</a>',
}

# --- Copernicus DEM GLO-30 (calculation DEM) --------------------------------
COPERNICUS_DEM_URL = ("https://copernicus-dem-30m.s3.amazonaws.com/"
                      "Copernicus_DSM_COG_10_{lat}_00_{lon}_00_DEM/Copernicus_DSM_COG_10_{lat}_00_{lon}_00_DEM.tif")
COPERNICUS_DEM = {
    "source": "Copernicus DEM GLO-30 (public release), AWS Open Data bucket copernicus-dem-30m",
    "licence": "Copernicus DEM GLO-30 public licence: free of charge, attribution required "
               "(operator: check the current licence text on the Copernicus/ESA site).",
    "attribution": "Copernicus DEM GLO-30 © DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH "
                   "2014-2018 provided under COPERNICUS by the European Union and ESA; all rights reserved",
    "vertical_datum": "EGM2008 geoid (orthometric heights, metres)",
    "notes": "Digital Surface Model (includes buildings/canopy). Pixel-is-point grid, 1 arc-second "
             "(longitude spacing grows above 50° latitude). Some tiles are not in the public release.",
}

# SRTM fallback: AWS Terrain Tiles 'skadi' (1 arc-second HGT tiles, no auth).
SRTM_SKADI_URL = "https://elevation-tiles-prod.s3.amazonaws.com/skadi/{lat}/{lat}{lon}.hgt.gz"
SRTM = {
    "source": "AWS Terrain Tiles (Mapzen/Tilezen 'skadi' HGT, 1 arc-second), SRTM-derived blend",
    "licence": "Mixed public sources; see https://github.com/tilezen/joerd/blob/master/docs/attribution.md "
               "(operator: verify for your area).",
    "attribution": "Elevation: SRTM (NASA/USGS) and other sources via Mapzen Terrain Tiles",
    "vertical_datum": "EGM96 geoid (SRTM), metres",
    "notes": "Fallback. Raw NASA SRTMGL1 from Earthdata needs a (free) Earthdata login; pass such "
             "files with --dem-file instead.",
}

DEM_FILE = {
    "source": "operator-supplied DEM files",
    "licence": "as supplied by the operator",
    "attribution": "set by the operator",
    "vertical_datum": "as supplied",
}

# --- ESA WorldCover 10 m (clutter) -------------------------------------------
WORLDCOVER_URL = ("https://esa-worldcover.s3.eu-central-1.amazonaws.com/{version}/{year}/map/"
                  "ESA_WorldCover_10m_{year}_{version}_{tile}_Map.tif")
WORLDCOVER_VERSIONS = {"2021": "v200", "2020": "v100"}
WORLDCOVER = {
    "source": "ESA WorldCover 10 m {year} {version}",
    "licence": "CC BY 4.0",
    "attribution": "© ESA WorldCover project {year} / Contains modified Copernicus Sentinel data ({year}) "
                   "processed by ESA WorldCover consortium",
    "classes": {
        "0": "No data", "10": "Tree cover", "20": "Shrubland", "30": "Grassland", "40": "Cropland",
        "50": "Built-up", "60": "Bare / sparse vegetation", "70": "Snow and ice", "80": "Permanent water bodies",
        "90": "Herbaceous wetland", "95": "Mangroves", "100": "Moss and lichen",
    },
}

# --- Satellite ----------------------------------------------------------------
SATELLITE_UNVERIFIED = ("UNVERIFIED: the operator must check the licence and the tile-service terms of "
                        "the chosen mosaic before publishing (non-commercial use) and confirm it with "
                        "--sat-licence-checked.")

# The mosaics the satellite step knows. Name, licence and attribution always come from this table,
# never from the command line; a mosaic that is not listed here is added here. Entries with a
# template are XYZ services the step may download from (--sat-provider); every entry can also
# describe a raster the operator put in /data/inputs (--sat-file with --sat-mosaic).
# The licence notes are what the publishers state; the operator still checks the current terms.
_EOX = "https://tiles.maps.eox.at/wmts/1.0.0/{layer}/default/g/{{z}}/{{y}}/{{x}}.jpg"
_EOX_ATTRIBUTION = ("Sentinel-2 cloudless - https://s2maps.eu by EOX IT Services GmbH "
                    "(Contains modified Copernicus Sentinel data {data})")
SATELLITE_MOSAICS = {
    "eox-s2cloudless-2016": {
        "name": "Sentinel-2 cloudless 2016 by EOX",
        "template": _EOX.format(layer="s2cloudless_3857"),
        "licence": "CC BY 4.0",
        "attribution": _EOX_ATTRIBUTION.format(data="2016 & 2017"),
    },
    **{
        f"eox-s2cloudless-{year}": {
            "name": f"Sentinel-2 cloudless {year} by EOX",
            "template": _EOX.format(layer=f"s2cloudless-{year}_3857"),
            "licence": "CC BY-NC-SA 4.0 (non-commercial)",
            "attribution": _EOX_ATTRIBUTION.format(data=year),
        }
        for year in range(2018, 2024)
    },
    "copernicus-sentinel-2": {
        "name": "Copernicus Sentinel-2 imagery",
        "licence": "Copernicus Sentinel data terms: free, full and open access; attribution required",
        "attribution": "Contains modified Copernicus Sentinel data {year}",
    },
}
SATELLITE_PROVIDERS = {key: m for key, m in SATELLITE_MOSAICS.items() if "template" in m}
