# Changelog

## [unreleased]

### Added
- Mapsplit (.msf) source data support
- `obj:MinZoom(z)` to set the minimum zoom at which a feature will be rendered
- Set minimum zooms for placenames, waterways, and landcover in OpenMapTiles processing (@typebrook, @systemed)
- Render roads under construction on OpenMapTiles processing (@meromisi, @Beck-berry)

### Changed
- Remove Lua scale functions now that we return metres
- Write OpenMapTiles placenames depending on zoom
- Use boost::geometry::intersection for clipping (faster than clipper)
- Consistently use 1TBS in source
- Only output validity errors in verbose mode

### Fixed
- Fix waterway handling in OpenMapTiles processing (@leonardehrenfried)
- Don't filter out ABCA areas (@rdsa)

## [1.6.0] - 2020-05-22

### Added
- Specify `source_columns: true` for shapefiles to import all attributes
- Support creating tiles from shapefiles only (i.e. no .osm.pbf)
- `attribute_function` to rewrite shapefile attributes from Lua
- Improved diagnostics for invalid multipolygons
- Output shapefile layer names when reading
- Report memory usage in verbose mode
- Out-of-the-box test tileserver and OpenMapTiles-compatible resources

### Changed
- Rewrite OpenMapTiles-compatible processing (@systemed, @sasfeat, @typebrook, @leonardehrenfried)
- `--combine` flag now off by default
- 32-bit ways and 16-bit tile index by default, change with `-DFAT_WAYS -DFAT_TILE_INDEX`
- Use tsl::sparse_map instead of std::unordered_map for ~7% memory saving
- Don't write invalid small polygons
- Speedup for shapefile reading (@TimSC)
- Move to Github Actions instead of Travis (@leonardehrenfried)
- Reduce size of Docker image, use Ubuntu 20.04 (@leonardehrenfried)
- Improve area and length calculation (@typebrook, @systemed)
- Overwrite existing .mbtiles file by default

### Fixed
- CMake build fixed (@ogre)
- Compatibility with pre-1.59 Boost Geometry
- Typo fixes (@bezineb5)
- Fix return value for --help (@typebrook)
- Use supercover (modified Bresenham) algorithm for which tiles are affected by diagonal lines

## [1.5.0] - 2018-02-18

### Added
- Support mbtiles 1.3 specification
- Write `extent` field to tiles
- Ability to specify MVT version (defaults to 2)
- OpenMapTiles-like Lua/JSON files (@TimSC)
- Dockerfile (@thomasbrueggemann)
- Better error messages (@TimSC)
- Support 64-bit way IDs (@TimSC)

### Changed
- Significant refactoring (@TimSC)

### Fixed
- Robustness fixes and error checking for invalid geometries, using clipper inter alia (@TimSC)
- Don't break if config files not found
- Don't break on massive .pbfs (e.g. France extract)
- Don't break if .pbf only contains nodes, not ways
- Fix build issues on some versions of OS X
- Makefile fixes (@pnorman, @thomersch)

## [1.4.0] - 2016-11-07

### Added
- Use threads when creating output tiles for massive speedup (@grafi-tt)
- Bundle kaguya - no need for Luabind any more
- Report how many output objects were stored
- Add glug support to mbtileserver

### Changed
- `way:FindContaining()` now returns a Lua table rather than an iterator

### Fixed
- Polygon filling algorithm rewritten to work consistently (@grafi-tt)

### Removed
- Mapbox Studio-compatible file layout removed (licensing debatable)
- Vagrantfile removed (dependency problems)

## [1.3.0] - 2016-07-11

### Added
- Add cmake scripts and support MSVC on Windows (@alex85k)
- Support `way:IsClosed()`, `way:Area()`, `way:Length()`, and `way:ScaleToKm()` (@grafi-tt)
- Optionally call lua functions `init_function()` and `exit_function()` (@tinoue)
- Support `simplify_ratio`, and calculate the actual simplify level by
  the formula `simplify_level * pow(simplify_ratio, (simplify_below-1) - <current zoom>)` (@tinoue)
- Support `simplify_length`, that is simplify threshold in meters, instead of in degrees (its length changes corresponding to the latitude) (@grafi-tt)
- Support 64-bit node IDs, with compile-time flag to use 32-bit (@systemed)
- Merge polygons with identical attributes (@grafi-tt)
- Error-handling for shapefile polygons and non-existent Lua layers (@grafi-tt)
- Support shapefile polygons with multiple exterior rings (@fofanov)

### Changed
- Optimized SQLite output (@grafi-tt)
- Refactored OSM object implementation (@grafi-tt)

### Fixed
- Add initialization to database class (avoid crash on shutdown) (@alex85k)
- Documentation issues (@AndreMiras, @rory)
- Clip shapefile geometries to tile boundaries (@grafi-tt)

## [1.2.0] - 2015-10-08

### Added
- Load shapefiles into layers
- Spatial queries (Intersects, FindIntersecting) on shapefiles
- Choose deflate, gzip or no compression (@tinoue)
- Show trace on Luabind errors (@tinoue)

### Changed
- Suppress "missing way" errors unless --verbose specified

### Fixed
- Die less horribly on Lua syntax errors
- Don't add attributes if no Layer set
- AttributeNumeric error (@tinoue)

## [1.1.0] - 2015-09-28

### Added
- `LayerAsCentroid` method to write centroid of polygons (for labelling and POIs)
- Option: simplify geometries on output
- Option: `write_to` combines multiple input layers in one output layer
- Option: gzip output compression
- Option: user-specified output metadata
- Vagrant config to ease creating VMs (@zerebubuth)
- Compile sources to .o temporary files, for faster recompiles (@zerebubuth)
- Lua/JSON config for Mapbox GL style-compatible output (@flamed0011)
- Simple Ruby .mbtiles server for testing

### Changed
- Store nodes with projected latitudes
- 10% speedup by using unordered_map (@zerebubuth)

### Fixed
- Don't die when `keys_vals` is empty in source .pbf (fixes bbbike/metro compatibility)
- Code correctness, esp. avoiding reallocating arrays (@zerebubuth)
- Build improvements (@zerebubuth)
- Documentation improvements

## [1.0.0] - 2015-06-29

### Added
- Initial release
