#!/usr/bin/ruby

# Vector tile server
#
# This serves individual .pbf tiles from an .mbtiles file, plus
# any static files in the static/ directory. Use it for testing tilemaker output
# with a renderer such as Mapbox GL. (Your .mbtiles should be
# created with compression enabled.)
#
# Standalone syntax:
#   ruby server.rb path_to.mbtiles
#
# It can also be run under Phusion Passenger.

require 'sqlite3'
require 'cgi'
require 'passenger'
begin; require 'glug'	# Optional glug dependency
rescue LoadError; end 	#  |

class MapServer

	CONTENT_TYPES = {
		json: "application/json",
		png: "image/png",
		pbf: "application/octet-stream",
		css: "text/css",
		js: "application/javascript"
	}
	
	EMPTY_TILE = ["1F8B0800FA78185E000393E2E3628F8F4FCD2D28A9D46850A86002006471443610000000"].pack('H*')
	EMPTY_FONT = ["0A1F0A124D6574726F706F6C697320526567756C61721209313533362D31373931"].pack('H*')

	def initialize(mbtiles, max_age=604800)
		@@mbtiles = mbtiles
		@@max_age = max_age
		MapServer.connect
	end

	def self.connect
		# @@db = SQLite3::Database.new(@@mbtiles)
		Dir.chdir("static") unless Dir.pwd.include?("static")
		self
	end
	
	def read_metadata
		md = {}
		@@db.execute("SELECT name,value FROM metadata").each do |row|
			k,v = row
			md[k] = k=='json' ? JSON.parse(v) : v
		end
		md
	end
	
	def call(env)
		path = CGI.unescape( (env['REQUEST_PATH'] || env['REQUEST_URI']).sub(/^\//,'') )
		if path.empty? then path='index.html' end

		if path =~ %r!^/?(\d+)/(\d+)/(\d+).*\.pbf!
			# Serve .pbf tile from mbtiles
			z,x,y = $1.to_i, $2.to_i, $3.to_i
			tms_y = 2**z - y - 1
			res = @@db.execute("SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?", [z, x, tms_y])
			blob = res.length==0 ? EMPTY_TILE : res[0][0]
			['200', {
				'Content-Type'    => 'application/x-protobuf', 
				'Content-Encoding'=> 'gzip', 
				'Content-Length'  => blob.bytesize.to_s, 
				'Cache-Control'   => "max-age=#{@@max_age}",
				'Access-Control-Allow-Origin' => '*'
			}, [blob]]

		elsif path=='metadata'
			# Serve mbtiles metadata
			['200', {'Content-Type' => 'application/json', 'Cache-Control' => "max-age=#{@@max_age}" }, [read_metadata.to_json]]

#		elsif ARGV.include?(path.sub(/\.json$/,'.glug'))
#			# Convert .glug style to .json
#			# ****** fixme
#			glug = Glug::Stylesheet.new { instance_eval(File.read( path.sub(/\.json$/,'.glug') )) }.to_json
#			['200', {'Content-Type' => 'application/json' }, [glug] ]

		elsif File.exist?(path)
			# Serve static file
			ct = path.match(/\.(\w+)$/) ? (CONTENT_TYPES[$1.to_sym] || 'text/html') : 'text/html'
			['200', {'Content-Type' => ct, 'Cache-Control' => "max-age=#{@@max_age}"}, [File.read(path)]]

		elsif path=~/font.+\.pbf/
			# Font not found so send dummy file
			['200', {
				'Content-Type' => 'application/x-protobuf',
				'Content-Length' => EMPTY_FONT.bytesize.to_s,
				'Access-Control-Allow-Origin' => '*'
			}, [EMPTY_FONT]]

		else
			# Not found
			puts "Couldn't find #{path}"
			['404', {'Content-Type' => 'text/html'}, ["Resource at #{path} not found"]]
		end
	end

	# Start server

	if defined?(PhusionPassenger)
		puts "Starting Passenger server"
		PhusionPassenger.on_event(:starting_worker_process) do |forked|
			if forked then MapServer.connect end
		end

	else
		puts "Starting local server"
		require 'rack'

		server = MapServer.new(ARGV[0],0)
		app = Proc.new { |env| server.call(env) }
		Rack::Handler::WEBrick.run(app)
	end
end
