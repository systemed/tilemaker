# Changelog

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
