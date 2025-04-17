## Configuring tilemaker

Vector tiles contain (generally thematic) 'layers'. For example, your tiles might contain river, cycleway and railway layers.

You'll assign OpenStreetMap data into layers by making decisions based on their tags. You might put anything with a `highway=` tag into the roads layer, anything with a `railway=` tag into the railway layer, and so on.

In tilemaker, you achieve this by writing a short script in the Lua programming language. Lua is a simple and fast language used by several other OpenStreetMap tools, such as the OSRM routing engine and osm2pgsql.

In addition, you supply tilemaker with a JSON file which lists the layers, and specifies certain global settings for your tileset.

### A note on zoom levels

Because vector tiles are so efficiently encoded, you generally don't need to create tiles above (say) zoom level 14. Instead, your renderer will use the data in the z14 tiles to generate z15, z16 etc. (This is called 'overzooming'.)

So when you set a maximum zoom level of 14 in tilemaker, this doesn't mean you're restricted to displaying maps at z14. It just means that tilemaker will create z14 tiles, and it's your renderer's job to use these tiles to draw the most detailed maps.

### Learning by doing

This file is a complete reference to tilemaker's configuration. Impatient and want to get your hands dirty straightaway? Look in the `resources/` directory for config-example.json and process-example.lua. This is a really simple config example. Experiment with changing the layer descriptions in the JSON, and the feature logic in the Lua script.

Make sure you've also read VECTOR_TILES.md (in `docs/`). This explains how tilemaker (which just generates the tiles) works together with a rendering library (like Maplibre) and stylesheet.

## JSON configuration reference

### Basic configuration

The JSON config file sets out the layers you'll be using, and which zoom levels they apply to. For example, you might want to include your roads layer in your z12-z14 tiles, but your buildings at z14 only.

It also includes these global settings:

* `minzoom` - the minimum zoom level at which any tiles will be generated
* `maxzoom` - the maximum zoom level at which any tiles will be generated
* `basezoom` - the zoom level for which tilemaker will generate tiles internally (should usually be the same as `maxzoom`)
* `include_ids` - whether you want to store the OpenStreetMap IDs for each way/node within your vector tiles
* `compress` - whether to compress vector tiles (Any of "gzip","deflate" or "none"(default))
* `combine_below` - whether to merge adjacent linestrings of the same type: will be done at zoom levels below that specified here (e.g. `"combine_below": 14` to merge at z1-13)
* `name`, `version` and `description` - about your project (these are written into the MBTiles file)
* `high_resolution` (optional) - whether to use extra coordinate precision at the maximum zoom level (makes tiles a bit bigger)
* `bounding_box` (optional) - the bounding box to output, in [minlon, minlat, maxlon, maxlat] order
* `default_view` (optional) - the default location for the client to view, in [lon, lat, zoom] order (MBTiles only)
* `mvt_version` (optional) - the version of the [Mapbox Vector Tile](https://github.com/mapbox/vector-tile-spec) spec to use; defaults to 2

A typical config file would look like this:

    {
      "layers": {
        "roads": { "minzoom": 12, "maxzoom": 14 },
        "buildings": { "minzoom": 14, "maxzoom": 14 },
        "pois": { "minzoom": 13, "maxzoom": 14 }
      },
      "settings": {
        "minzoom": 12,
        "maxzoom": 14,
        "basezoom": 14,
        "include_ids": false,
        "compress": "gzip",
        "name": "Tilemaker example",
        "version": "0.1",
        "description": "Sample vector tiles for tilemaker"
      }
    }

The order of layers will be carried forward into the vector tile.

Layers with `write_to` set must appear after the layers they're writing into. An incorrect order will result in "the layer to write doesn't exist".

All options are compulsory unless stated otherwise. If tilemaker baulks at the JSON file, check everything's included, and run it through an online JSON validator to check for syntax errors.

By default tilemaker expects to find this file at config.json, but you can specify another filename with the `--config` command-line option.

### Advanced layer configuration

You can add optional parameters to layers:

* `write_to` - write features to a previously named layer
* `simplify_below` - simplify features below this zoom level
* `simplify_level` - how much to simplify features (in degrees of longitude) on the zoom level `simplify_below-1`
* `simplify_length` - how much to simplify features (in kilometers) on the zoom level `simplify_below-1`, preceding `simplify_level`
* `simplify_ratio` - (optional: the default value is 2.0) the actual simplify level will be `simplify_level * pow(simplify_ratio, (simplify_below-1) - <current zoom>)`
* `simplify_algorithm` - which simplification algorithm to use (defaults to Douglas-Peucker; you can also specify `"visvalingam"`, which can be better for landuse and similar polygons)
* `filter_below` - filter areas by minimum size below this zoom level
* `filter_area` - minimum size (in square degrees of longitude) for the zoom level `filter_below-1`
* `feature_limit` - restrict the number of features written to each tile
* `feature_limit_below` - restrict only below this zoom level
* `combine_polygons_below` - merge adjacent polygons with the same attributes below this zoom level
* `combine_points` - merge points with the same attributes (defaults to `true`: specify `false` to disable)
* `z_order_ascending` - sort features in ascending order by a numeric value set in the Lua processing script (defaults to `true`: specify `false` for descending order)

`write_to` enables you to combine different layer specs within one outputted layer. For example:

    "layers": {
      "roads": { "minzoom": 12, "maxzoom": 14 },
      "low_roads": { "minzoom": 9, "maxzoom": 11, "write_to": "roads", "simplify_below": 12, "simplify_level": 0.0001 }
    }

This would combine the `roads` (z12-14) and `low_roads` (z9-11) layers into a single `roads` layer on writing, with simplified geometries for `low_roads`.

(See also 'Shapefiles and GeoJSON' below.)

### Additional metadata

Tilemaker writes a `json` metadata field containing a `vector_layers` key, whose value is an array of JSON objects describing each layer and its attributes. This is part of the MBTiles 1.3 spec and required by certain clients.

If you need to add additional metadata fields to your .mbtiles output, include the keys/values as an (optional) "metadata" entry under "settings". These will usually be string key/value pairs. (The value can also be another JSON entity - hash, array etc. - in which case it'll be encoded as JSON when written into the .mbtiles metadata table.)

For example:

    {
      "layers": { ... },
      "settings": { ... ,
        "metadata": {
          "author": "THERE Data Inc",
          "licence": "ODbL 1.1",
          "layer_order": { "water": 1, "buildings": 2, "roads": 3 }
        }
      }
    }
	
## Lua processing reference

Your Lua file can supply these functions for tilemaker to call:

1. (optional) `node_keys`, a list of those OSM tags which indicate that a node should be processed
2. (optional) `way_keys`, a list of those OSM tags which indicate that a way should be processed
3. `node_function()`, a function to process an OSM node and add it to layers
4. `way_function()`, a function to process an OSM way and add it to layers
5. (optional) `init_function(name, is_first)`, a function to initialize Lua logic
6. (optional) `exit_function`, a function to finalize Lua logic (useful to show statistics)
7. (optional) `relation_scan_function`, a function to determine whether your Lua file wishes to process the given relation 
8. (optional) `relation_function`, a function to process an OSM relation and add it to layers
9. (optional) `attribute_function`, a function to remap attributes from shapefiles

### Principal Lua functions

`node_function` and `way_function` do the main work. They are called with an OSM object; you then inspect the tags of that object, and put it in your vector tiles' layers based on those tags. In essence, the process is:

* look at tags
* if tags meet criteria, write to a layer
* (optionally) add attributes (= vector tile metadata/tags) 

Note the order: you write to a layer first, then set attributes after.

To do that, you use these methods:

* `Find(key)`: get the value for a tag, or the empty string if not present. For example, `Find("railway")` might return "rail" for a railway, "siding" for a siding, or "" if it isn't a railway at all.
* `Holds(key)`: returns true if that key exists, false otherwise.
* `AllKeys()`: returns a table (array) containing all the OSM tag keys.
* `AllTags()`: returns a table containing all the OSM tags.
* `Layer(layer_name, is_area)`: write this node/way to the named layer. This is how you put objects in your vector tile. is_area (true/false) specifies whether a way should be treated as an area, or just as a linestring.
* `LayerAsCentroid(layer_name, algorithm, role, role...)`: write a single centroid point for this way to the named layer (useful for labels and POIs). Only the first argument is required. `algorithm` can be "polylabel" (default) or "centroid". The third arguments onwards specify relation roles: if you're processing a multipolygon-type relation (e.g. a boundary) and it has a "label" node member, then by adding "label" as an argument here, this will be used in preference to the calculated point.
* `Attribute(key,value,minzoom)`: add an attribute to the most recently written layer. Argument `minzoom` is optional, use it if you do not want to write the attribute on lower zoom levels.
* `AttributeNumeric(key,value,minzoom)`, `AttributeBoolean(key,value,minzoom)`: for numeric/boolean columns.
* `Id()`: get the OSM ID of the current object.
* `IsClosed()`: returns true if the current object is a closed area.
* `IsMultiPolygon()`: returns true if the current object is a multipolygon.
* `ZOrder(number)`: Set a numeric value (default 0) used to sort features within a layer. Use this feature to ensure a proper rendering order if the rendering engine itself does not support sorting. Sorting is not supported across layers merged with `write_to`. Features with different z-order are not merged if `combine_below` or `combine_polygons_below` is used. Use this in conjunction with `feature_limit` to only write the most important (highest z-order) features within a tile. (Values can be -50,000,000 to 50,000,000 and are lossy, particularly beyond -1000 to 1000.)
* `MinZoom(zoom)`: set the minimum zoom level (0-15) at which this object will be written. Note that the JSON layer configuration minimum still applies (so `:MinZoom(5)` will have no effect if your layer only starts at z6).
* `Length()` and `Area()`: return the length (metres)/area (square metres) of the current object. Requires Boost 1.67+.
* `Centroid()`: return the lat/lon of the centre of the current object as a two-element Lua table (element 1 is lat, 2 is lon).

The simplest possible function, to include roads/paths and nothing else, might look like this:

```lua
    function way_function()
      local highway = Find("highway")
      if highway~="" then
        Layer("roads", false)
        Attribute("name", Find("name"))
        Attribute("type", highway)
      end
    end
```

Take a look at the supplied process.lua for a simple example, or the more complex OpenMapTiles-compatible script in `resources/`. You can specify another filename with the `--process` option.

If your Lua file causes an error due to mistaken syntax, you can test it at the command line with `luac -p filename`. Three frequent Lua gotchas: tables (arrays) start at 1, not 0; the "not equal" operator is `~=` (that's the other way round from Perl/Ruby's regex operator); and `if` statements always need a `then`, even when written over several lines.

> [!IMPORTANT]  
> If you are upgrading from a version of tilemaker before 3.0, note that you now write methods simply as `Find("key")` or `Layer("name")` etc., not as `way:Find("key")` or `way:Layer("name")`.

### Other Lua functions and lists

`node_keys` is a simple list (or in Lua parlance, a 'table') of OSM tags. If a node has one of those keys, it will be processed by `node_function`; if not, it'll be skipped. For example, if you wanted to show highway crossings and railway stations, it should be `{ "highway", "railway" }`. (This avoids the need to process the vast majority of nodes which contain no important tags at all.)

`way_keys` is similar, but for ways. For ways, you may also wish to express the filter in terms of the tag value, or as an inversion. For example, to exclude buildings: `way_keys = {"~building"}`. To build a map only of major roads: `way_keys = {"highway=motorway", "highway=trunk", "highway=primary", "highway=secondary"}`

`init_function(name, is_first)` and `exit_function` are called at the start and end of processing (once per thread). You can use this to output statistics or even to read a small amount of external data. `is_first` will be true only the first time `init_function` is called.

Other functions are described below and in RELATIONS.md.

### Relations

Tilemaker handles multipolygon relations natively. The combined geometries are processed as ways (i.e. by `way_function`), so if your function puts buildings in a 'buildings' layer, tilemaker will cope with this whether the building is mapped as a simple way or a multipolygon. The only difference is that they're given an artificial ID. Multipolygons are expected to have tags on the relation, not the outer way.

Working with other types of relations (e.g. routes) is documented in RELATIONS.md.

## Shapefiles and GeoJSON

Tilemaker chiefly works with OpenStreetMap .osm.pbf data, but you can also bring in external data in shapefile or GeoJSON format. This is useful for rarely-changing data such as coastlines and built-up area outlines.

Shapefiles and GeoJSON are imported directly in your layer config like this:

    "urban_areas": {
      "minzoom": 11, "maxzoom": 14,
      "source": "data/urban_areas.shp",
      "simplify_below": 13, "simplify_level": 0.0003
    },
    "bridges": {
      "minzoom": 13, "maxzoom": 14,
      "source": "data/Bridges_WGS84.shp", 
      "source_columns": ["SAP_DESCRI"]
    }

You can specify attribute columns to import using the `source_columns` parameter, and they'll be available within your vector tiles just as any OSM tags that you import would be. To import all columns, use `"source_columns": true`.

Limited Lua transformations are available for these files. You can supply an `attribute_function(attr,layer)` which takes a Lua table (hash) of shapefile attributes, as already filtered by `source_columns`, and the layer name. It must return a table (hash) of the vector tile attributes to set.

To set the minimum zoom level at which an individual feature is rendered, use `attribute_function` to set a `_minzoom` value in your return table.

Shapefiles/GeoJSON **must** be in WGS84 projection, i.e. pure latitude/longitude. (Use ogr2ogr to reproject them if your source material is in a different projection.) They will be clipped to the bounds of the first .pbf that you import, unless you specify otherwise with a `bounding_box` setting in your JSON file.

### Lua spatial queries

When processing OSM objects with your Lua script, you can perform simple spatial queries against a shapefile/GeoJSON layer. Let's say you have the following shapefile layer containing country polygons, each one named with the country name:

    "countries": {
      "minzoom": 2, "maxzoom": 10,
      "source": "data/country_polygons.shp",
      "index": true, "index_column": "NAME"
    }

You can then find out whether a node is within one of these polygons using the `Intersects` method:

    if Intersects("countries") then print("Looks like it's on land"); end

Or you can find out what country(/ies) the node is within using `FindIntersecting`, which returns a table:

    names = FindIntersecting("countries")
    print(table.concat(name,","))

To enable these functions, set `index` to true in your shapefile layer definition. `index_column` is not needed for `Intersects` but required for `FindIntersecting`.

`CoveredBy` and `FindCovering` work similarly but check if the object is covered by a shapefile layer object.

`AreaIntersecting` returns the area of the current way's intersection with the shapefile layer. You can use this to find whether a water body is already represented in a shapefile ocean layer.

### Lua key/value store

tilemaker has a simple key/value store accessible from Lua which you can use to bring in external data. The same store is used across all processing threads.

Read your data from file, using [Lua's I/O functions](https://www.lua.org/pil/21.1.html), in `init_function` (checking that `is_first` is set for the first run only). Set a key/value pair with `SetData(key,value)` - for example `SetData("name","Bill")`. Both key and value should be strings.

You can then retrieve the value within `way_function` or similar with `GetData(key)`. If no value was found, the empty string is returned.