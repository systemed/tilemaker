#!/usr/bin/ruby

# mbtileserver

# This serves individual .pbf tiles from an .mbtiles file, plus
# any static files you specify. Use it for testing tilemaker output
# with a renderer such as Mapbox GL. (Your .mbtiles should be
# created with compression enabled.)

# Syntax:
#   ruby mbtileserver.rb path_to.mbtiles otherstaticfile.1 otherstaticfile.2

# Example:
#   ruby mbtileserver.rb oxfordshire.mbtiles *.json sprites*
# will make files accessible at:
#   http://localhost:8080/14/8124/5421.output.pbf
#   http://localhost:8080/style.json
# etc.

require 'rack'
require 'sqlite3'

begin; require 'glug'	# Optional glug dependency
rescue LoadError; end 	#  |

db = SQLite3::Database.new(ARGV[0])
content_types = {
	json: "application/json",
	png: "image/png",
	pbf: "application/octet-stream"
}

app = Proc.new do |env|
	path = env['REQUEST_PATH'].sub(/^\//,'')

	if path =~ %r!/(\d+)/(\d+)/(\d+).*\.pbf!
		# Serve .pbf tile from mbtiles
		z,x,y = $1.to_i, $2.to_i, $3.to_i
		tms_y = 2**z - y - 1
		res = db.execute("SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?", [z, x, tms_y])
		if res.length>0
			puts "Serving #{z}/#{x}/#{y}"
			blob = res[0][0]
			['200', {'Content-Type'=>'application/x-protobuf', 'Content-Encoding'=>'gzip', 'Content-Length'=>blob.bytesize.to_s }, [blob]]
		else
			puts "Empty #{z}/#{x}/#{y}"
			['200', {'Content-Type'=>'application/x-protobuf', 'Content-Length'=>'0' }, []]
		end

	elsif ARGV.include?(path)
		# Serve static file
		ct = path.match(/\.(\w+)$/) ? (content_types[$1.to_sym] || 'text/html') : 'text/html'
		['200', {'Content-Type' => ct}, [File.read(path)]]

	elsif ARGV.include?(path.sub(/\.json$/,'.glug'))
		# Convert .glug style to .json
		glug = Glug::Stylesheet.new { instance_eval(File.read( path.sub(/\.json$/,'.glug') )) }.to_json
		['200', {'Content-Type' => 'application/json' }, [glug] ]

	else
		# Not found
		['404', {'Content-Type' => 'text/html'}, ["Resource at #{path} not found"]]
	end
end

Rack::Handler::WEBrick.run app
