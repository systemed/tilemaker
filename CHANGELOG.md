# Changelog

## [3.0.0] - 2024-01-15

3.0 is a major release that significantly reduces tilemaker's memory footprint and improves running time. Note that it has __breaking changes__ in the way you write your Lua scripts (`way:Layer` becomes simply `Layer`, and so on).

### Added
- PMTiles output (@systemed)
- C++ tilemaker-server for quick prototyping (@bdon)
- GeoJSON supported as an alternative to shapefiles (@systemed)
- Support nodes in relations and relation roles (@cldellow)
- Nested relations support (@systemed/@cldellow)
- `LayerAsCentroid` can use positions from relation child nodes (@cldellow)
- Add polylabel algorithm to `LayerAsCentroid` (@cldellow)
- Filter input .pbf by way keys (@cldellow)
- GeoJSON writer for debugging (@systemed)
- Warn about PBFs with large blocks (@cldellow)
- Unit tests for various features (@cldellow)
- `RestartRelations()` to reset relation iterator (@systemed)
- Per-layer, zoom-dependent feature_limit (@systemed after an original by @keichan34)
- Report OSM ID on Lua processing error (@systemed)
- Docker OOM killer warning (@Firefishy)
- Push Docker image to Github package (@JinIgarashi)
- Support `type=boundary` relations as native multipolygons (@systemed)

### Changed
- __BREAKING__: Lua calls use the global namespace, so `Layer` instead of `way:Layer` etc. (@cldellow)
- __BREAKING__: Mapsplit (.msf) support removed (@systemed)
- Widespread speed improvements (@cldellow, @systemed)
- Reduced memory consumption (@cldellow)
- protobuf dependency removed: protozero/vtzero used instead (@cldellow)
- Better Lua detection in Makefile (@systemed)
- z-order is now a lossy float: compile-time flag not needed (@systemed)
- --input and --output parameter names no longer required explicitly (@systemed)
- Docker image improvements (@Booligoosh)

### Fixed
- Improved polygon correction (@systemed)
- Add missing attributes to OMT layers (@Nakaner)
- Use different OSM tags for OMT subclasses (@Nakaner)
- Add access and mtb_scale attributes to OMT (@dschep)
- Fix CMake build on Arch Linux (@holzgeist)


## [2.4.0] - 2023-03-28

### Added
- Option to reverse object sort order (@Nakaner, @systemed)
- Compile-time option to use floats for ZOrder (@Nakaner, @systemed)
- Advisory note if user tries to generate tiles at z16+ (@systemed)

### Changed
- Faster tile clipping (@systemed based on code by @mourner)
- Use rtree to index large polygons (@systemed, @kleunen)

### Fixed
- Update use of access in OpenMapTiles-compatible schema (@dschep)
- Align path/track transportation classes with OpenMapTiles (@dschep)
- Add missing paved/unpaved values as per OpenMapTiles (@dschep)


## [2.3.0] - 2023-03-08

### Added
- Send project name to init_function (@systemed)
- Remove zero-width spikes after simplification (@systemed)
- Remove multipolygon inners below filter area size (@systemed)

### Changed
- Move centroid and "no indexed layer" errors to verbose mode only (@systemed)
- Report missing layers consistently (@akx)
- Update Ruby server to Rack 3 (@typebrook)
- Move mmap shutdown to end of PBF reading (@systemed)

### Fixed
- Use std::ofstream instead of boost::filesystem (@milovanderlinden)
- Scale geometries before simplifying to avoid reintroducing self-intersections (@systemed)
- Fix manpage in makefiles (@xamanu)
- Intersect multipolygons part-by-part with clipping box to fix Boost.Geometry issue (@systemed)
- Windows issues (@roundby)
- Add libatomic for rare architectures (@xamanu)
- Ignore nodes in ways with --skip-integrity (@systemed)
- Correctly mask IDs for output with include_ids (@systemed)


## [2.2.0] - 2022-03-11

### Added
- Calculate center and write to metadata (@yuiseki)
- Option to use high-resolution geometries at max zoom (@systemed)
- Output slow geometries and allow user interrupt (@systemed, @billysan)
- Support osmium locations-on-ways format (@systemed)
- CORS support in server.rb (@Kimiru)

### Changed
- Faster multipolygon combining (@systemed)
- Faster multilinestring combining (@systemed)

### Fixed
- Correctly store and write points from .shp (@systemed)
- Relation scan is now thread-safe (@systemed)
- Remove unused variable in OMT profile (@leonardehrenfried)


## [2.1.0] - 2022-02-11

### Added
- Relation support via new Lua functions (@systemed)
- Restore --compact mode for memory-efficient sequential store (@kleunen)
- Give objects a ZOrder which is sorted on output (@Nakaner)
- Add man page (@xamanu)
- Configurable language support in OMT-compatible schema (@systemed)
- Support highway=pedestrian (@leonardehrenfried)
- New --skip-integrity option to disable way-node check (@systemed)
- New --bbox option which overwrites any other bounding box (@systemed)

### Changed
- Reduce Docker image size (@guillaumerose)
- Build no longer requires git (@xamanu)
- Faster multipolygon assembly (@systemed)
- Faster simplify (@kleunen)
- Faster shutdown and delete mmap file (@kleunen)
- Reduce memory usage by optimising OutputObject (@kleunen)
- Reduce memory usage by not storing ways unless used by relations (@systemed)
- Unbundle rapidjson and expect it as a dependency (@xamanu, @kleunen)
- simplify_level used consistently through OMT-compatible schema (@systemed)
- Use destdir and prefix variables in Makefile (@xamanu)

### Fixed
- Load JSON module in example Ruby server (@Silvercast)
- Support multiple types of entities in a single PBF block (@irnc)
- Correctly output OSM object IDs (@typebrook)
- Improve POI output in OMT-compatible schema (@systemed)
- Don't write 'meta'-layers (using write_to attribute) to metadata.json (@Nakaner)
- Handle nan issue in MinZoom/ZOrder with invalid values (@kleunen)
- Use real relation IDs in processing (@systemed)
- Support new homebrew paths on Apple Silicon Macs (@prebm)
- Improve Lua support in Makefile (@kleunen, @zidel)
- Clamp latitude to range valid for spherical Mercator (@kleunen)
- Documentation updates (@xamanu, @systemed)

## [2.0.0] - 2021-07-09

### Added
- Optionally use on-disk workspace with new --store option (@kleunen)
- Load .pbf in parallel (@kleunen)
- Static executable build for github CI (@kleunen)
- Mac and Windows CI builds (@kleunen)
- Write metadata.json for file output (@kleunen)
- Merge tile contents when using --merge switch
- Mapsplit (.msf) source data support
- `obj:MinZoom(z)` to set the minimum zoom at which a feature will be rendered
- `obj:Centroid` to get the central lat/lon of an OSM object
- `filter_below` to skip small areas at low zooms
- Make layer name available in shapefile `attribute_function`
- Set minimum zoom at which attributes are written
- Set minimum zoom for shapefile processing
- Set minimum zooms for placenames, waterways, buildings, and landcover in OpenMapTiles processing (@typebrook, @systemed)
- Render roads under construction on OpenMapTiles processing (@meromisi, @Beck-berry)
- Support any (post-5.1) version of Lua
- Build with Github Action (@typebrook)
- Use a shared key/value dictionary across OutputObjects to reduce memory usage (@kleunen)

### Changed
- C++14 required
- Remove Lua scale functions now that we return metres
- Improve OpenMapTiles tag processing (@leonardehrenfried, @typebrook, @systemed, @QuentinC, @keichan34)
- Use OpenMapTiles processing as default in tilemaker directory
- Change OpenMapTiles minzoom to 0
- Default simplify_ratio to 2
- Ignore Lake Saimaa and USFS National Forest complex polygons in OpenMapTiles script
- Rewrite linestring/polygon combining, with zoom level control (`combine_below` and `combine_polygons_below`)
- Use boost::geometry::intersection for clipping (faster than clipper)
- New simplify code (@kleunen)
- Use boost::asio::thread_pool for tile generation (@kleunen)
- Fallback to valid polygons if simplification produces invalid ones
- Consistently use 1TBS in source
- Only output validity errors in verbose mode
- Various speedups (don't add objects to output list that fail minZoom, optimise clipping)

### Fixed
- Don't filter out ABCA areas (@rdsa)
- Don't break with old versions of sqlite
- Don't generate tiles outside bounding box (@kleunen)
- Dissolve problematic geometries (@kleunen)
- Assign multipolygon inners to correct outers, including multiple way inners
- Significant performance improvements (@kleunen)
- Support nodes in LayerAsCentroid

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
