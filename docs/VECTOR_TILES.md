# An introduction to vector tiles

## The basics

Vector tiles are the modern way of rendering maps. Rather than sending a finished .png image over the wire, vector tiles encode all the geometry (shapes) and attributes (e.g. road names/types) in a binary format. Your browser or phone app then renders (draws) them on-screen. This has several advantages:

- Efficient storage, including for offline use
- On-the-fly restyling is possible
- Interactive maps where you can click on features
- Smooth rendering at intermediate zoom levels
- Rotated or perspective rendering

To create the vector tiles, you need software capable of reading OpenStreetMap data (in .pbf format), and slicing it up into vector tiles. This is where tilemaker comes in.

## The format

Vector tiles are usually encoded in .mvt format, for Mapbox Vector Tiles. The specification was drawn up by Mapbox, who offer a popular SaaS vector tile product. However, you don't need a Mapbox contract to use vector tiles from anyone else.

Often, rather than writing thousands of individual .mvt files, you'll store them together in an .mbtiles container. (Under the hood, this is an SQLite database.)

**Extra detail:** Vector tiles have the same boundaries as raster tiles. They're square tiles in the Web Mercator projection, where a zoom 2 tile covers twice the width and height of a zoom 3 tile, and so on. A zoom 0 tile covers much of the world, while a zoom 14 tile covers a small local area. Usually you won't generate vector tiles above zoom level 14 (sometimes 15), as these contain all the data needed for higher zoom levels anyway. (Note that zoom levels are off by one compared to raster tiles; a z13 vector tile is the same scale as a z14 raster tile.)

## Source data

You'll mostly use OpenStreetMap's openly-licensed map data to make your vector tiles. You get OSM data in a .pbf-format dump, rather than pulling it from an API. Several sites, such as [Geofabrik](https://download.geofabrik.de) and [BBBike](https://extract.bbbike.org), provide free country/city extracts of OSM data. (You can also download a dump of the whole planet [directly from OSM](https://planet.osm.org), but this is a massive file which is probably too much for tilemaker to process.)

You might use other openly-licensed sources for coastlines and small-scale landuse. Tilemaker can read such data in shapefile format.

## Schemas

You don't include every item of OpenStreetMap data in your vector tiles. Rather, you pick and choose which objects to include at which zoom level. You'll put different objects into layers, to make styling easier: for example, you might have a "roads" layer, a "landuse" layer and a "POIs" (points of interest) layer. These layer definitions are commonly known as a schema.

With tilemaker, you use a JSON file to set out your layers, and a script written in the Lua programming language to put objects in layers.

There's no one standard schema, but that used by the OpenMapTiles project is popular. Tilemaker includes ready-made JSON/Lua config files compatible with the OpenMapTiles schema, so you can use it out-of-the-box to generate OpenMapTiles-compatible vector tiles.

## Serving and rendering

Once you've generated your vector tiles, you need to render them on-screen. This is outside tilemaker's scope, but here's some pointers.

If you're serving them over the web, you'll need a server which accepts requests, and responds with the relevant tile. You'll usually do this by reading the record from the .mbtiles container (for which you can use a SQLite client library) and sending it back over HTTP. There's a Ruby example of how to do this in tilemaker's `server/` directory. Ready-made servers in other languages are available as open source, such as [mbtileserver](https://github.com/consbio/mbtileserver) (Go) and [tileserver-php](https://github.com/maptiler/tileserver-php), or you can use [tileserverless](https://github.com/geolonia/tileserverless) to serve direct from AWS.

It's then up to the client to render the tiles. There are a few libraries that do this, but the most popular and full-featured is that developed by Mapbox, usually known as Mapbox GL. The latest versions of this are closed-source and require a Mapbox contract, but the earlier open-source version has been forked and continues as [MapLibre GL](https://github.com/maplibre), which we recommend. There's a JavaScript version as well as an iOS/Android native version.

## Styles

The final ingredient is a styling file (stylesheet). This is a set of instructions telling the client how the vector tiles should be rendered: for example, "draw all rivers in blue" or "draw churches with a cross symbol".

Mapbox/MapLibre GL uses a JSON file for this, which in turn may reference external icon and font files. The example server with tilemaker includes one such style, from the OpenMapTiles project; they offer [several others](https://openmaptiles.org/styles/). You can also use [Maputnik](https://maputnik.github.io/editor/) to design your own styles visually, or programmatically with [Glug](https://github.com/systemed/glug).

**Extra detail:** You will sometimes see a "spec.json" or "tilejson" file. This is a way of specifying a link to your vector tile URL plus metadata such as minimum/maximum zoom in a single file. This is then referenced in the stylesheet, so the stylesheet knows where to pull the data from. But you can also specify this directly in the style file, and you may find that more straightforward.
