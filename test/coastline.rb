
	# Test coastline generation for given tiles
	# For each test case:
	# - run tilemaker
	# - extract tile using sqlite
	# - convert to GeoJSON
	# - read in GeoJSON
	# - calculate area and number of points
	# Requires vt2geojson

	# Run from tilemaker directory, i.e. ruby test/coastline.rb

	require 'fileutils'
	require 'sqlite3'
	require 'colorize'
	require 'json'
	require 'pp'
	require 'georuby'
	require 'geo_ruby/geojson'

	FileUtils.mkdir_p "test/tmp"
	
	def deg2rad(n); n*Math::PI/180.0 end
	def seg_calc(latp1,lon1,latp2,lon2)
		dx = lon1 - lon2
		t = latp1 * latp2
		2 * Math::atan2(t * Math::sin(dx), 1 + t * Math::cos(dx))
	end
	def polygon_area(points)
        sum = 0
		prev = points.last
        prev_latp = Math::tan((Math::PI / 2 - deg2rad(prev.y)) / 2)
        prev_lon  = deg2rad(prev.x)
		points.each do |point|
            latp = Math::tan((Math::PI / 2 - deg2rad(point.y)) / 2)
            lon  = deg2rad(point.x)
            sum += seg_calc(latp, lon, prev_latp, prev_lon)
            prev_latp = latp
            prev_lon  = lon
		end
		(6378.137 * 6378.137 * sum).abs
	end

	def coastline_test(name,bbox,tiles)
		puts "Running coastline tests for #{name}".white.on_black
		# Create the tiles
		system("tilemaker --config resources/config-coastline.json --process resources/process_coastline.lua --bbox #{bbox.join(',')} --output test/tmp/output.mbtiles")
		db = SQLite3::Database.new("test/tmp/output.mbtiles")
		tiles.each do |tile|

			# Get the tile
			z,x,y = tile
			puts "#{name} #{z}/#{x}/#{y}"
			tms_y = 2**z - y - 1
			res = db.execute("SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?", [z, x, tms_y])
			if res.length==0 then puts "No tile found".red; next end
			blob = res[0][0]
			File.write("test/tmp/output.pbf", blob)

			# Convert to GeoJSON and parse
			json_str = `vt2geojson -z #{z} -x #{x} -y #{y} test/tmp/output.pbf`
			geojson = GeoRuby::SimpleFeatures::Geometry.from_geojson(json_str)
			
			# Analyse features
			results = []
			geojson.features.each_with_index do |feature,feature_index|
				geom = feature.geometry
				if geom.is_a?(GeoRuby::SimpleFeatures::Polygon)
					polygons = [geom]
				elsif geom.is_a?(GeoRuby::SimpleFeatures::MultiPolygon)
					polygons = geom.geometries
				else
					puts "Feature #{feature_index} not recognised (#{geom.class.name})"
					pp geom
					next
				end
				polygons.each_with_index do |polygon,polygon_index|
					polygon.rings.each_with_index do |ring,ring_index|
						area = polygon_area(ring.points)
						print "Feature #{feature_index} polygon #{polygon_index} ring #{ring_index} "
						puts "has #{ring.points.count} points, #{area} sq km"
					end
				end
			end
		end
	end
	
	coastline_test("Athens",
		[23.69,37.37,24.61,38.07],
		[[8,145,98]])
