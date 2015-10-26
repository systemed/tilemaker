local string = require 'string'

-----------------------------------------
-- Settings                            --
--                                     --
-- debug_print: verbose mode           --
-----------------------------------------
local debug_print = false
local statistic_print = false

-----------------------------------------
-- Counter for statistics              --
-----------------------------------------
local counter = { }

-----------------------------------------
-- Helper functions                    --
--                                     --
-- Set to support key lookup           --
-----------------------------------------
function Set (list)
  local set = {}
  for _, l in ipairs(list) do set[l] = true end
  return set
end

function Statistics (tag, tagName)
	io.write(debug_print and tagName or "")
	if statistic_print then
		if counter[tagName] == nil then
			counter[tagName] = 0
		else
			counter[tagName] = counter[tagName] + 1
			local output_after = 100;
			if(counter[tagName] % output_after == 0) then
				local output = "processed <output_after> <tag_name> tags"
				local output = string.gsub(output, "<tag_name>", tagName)
				local output = string.gsub(output, "<output_after>", output_after)
				print(output)
			end
		end
	end
end

-----------------------------------------
-- node_keys is a list of OSM tag keys --
--                                     --
-- If a node has one of those keys,    --
-- it will be processed                --
-----------------------------------------
node_keys = {'place','capital','admin_level','name','aeroway','shop','amenity'}

-----------------------------------------
-- Filter Definitions                  --
--                                     --
-- these sets are filters for specific --
-- keys, which should not be displayed --
-- on th map (e.g. if they are empty)  --
-----------------------------------------
local amenity_exclude = Set { "", "car_sharing" }
local place_exclude = Set { "" }
local shop_exclude = Set { "" }
local landuse_exclude = Set { "" }
local natural_exclude = Set { "" }
local road_exclude = Set { "" }
local railway_exclude = Set { "" }
local aeroway_exclude = Set { "" }
local barrier_line_exclude = Set { "" }
local landuse_overlay_exclude = Set { "" }
local building_exclude = Set { "" }

-----------------------------------------
-- Icons supported by bright-v8 style  --
--                                     --
-- POIs without a corresponding icon   --
-- will use the marker icon            --
-----------------------------------------
local icons = Set {
	"aerialway", "airfield", "airport", "alcohol-shop", "america-football", "art-gallery", "bakery", "bank", "bar",
	"baseball", "basketball", "beer", "bicycle", "building", "bus", "cafe", "camera", "campsite", "car", "cemetery",
	"chemist", "cinema", "circle", "city", "clothing", "college", "commercial", "cricket", "cross", "dam",
	"danger", "dentist", "disability", "dog-park", "embassy", "emergency-telephone", "entrance", "farm", "fast-food",
	"ferry", "fire-station", "fuel", "garden", "gift", "golf", "grocery", "hairdresser", "harbor", "heart", "heliport",
	"hospital", "ice-cream", "industrial", "land-use", "laundry", "library", "lighthouse", "lodging",
	"london-underground", "marker", "marker-stroked", "minefield", "mobilephone", "monument",
	"motorway_1", "motorway_2", "motorway_3", "motorway_4", "motorway_5", "motorway_6", "museum", "music",
	"oil-well", "park2", "park", "parking", "parking-garage", "pharmacy", "pitch", "place-of-worship", "playground",
	"police", "polling-place", "post", "prison", "rail", "rail-above", "rail-light", "rail-metro", "rail-underground",
	"religious-christian", "religious-jewish", "religious-muslim", "restaurant", "roadblock", "rocket", "school",
	"scooter", "shop", "skiing", "slaughterhouse", "soccer", "square", "square-stroked", "star", "star-stroked", "suitcase",
	"swimming", "telephone", "tennis", "theatre", "toilets", "town", "town-hall", "triangle", "triangle", "village",
	"warehouse", "waste-basket", "water", "wave", "wetland", "zoo"
}

-- Initialize Lua logic

function init_function()
end

-- Finalize Lua logic()
function exit_function()
end

function node_function(node)
	-- labels
	local place = node:Find("place")
	local capital = node:Find("capital")
	local admin_level = node:Find("admin_level")
	local name = node:Find("name")
	local aeroway = node:Find("aeroway")
	local shop = node:Find("shop")
	local amenity = node:Find("amenity")

    -----------------
	-- place layer --
	-----------------
	if not place_exclude[place] then
		Statistics(node, "place")

		node:Layer("place_label", false)
		node:Attribute("name",name)
		-- the main field for styling labels for different kinds of places is type.
		-- possible values: 'city','town','village','hamlet','suburb','neighbourhood'
		node:Attribute("type",place)
		--The capital field allows distinct styling of labels or icons for the capitals of countries, regions, or states & provinces.
		-- 2=National capital, 3=Regional capital (uncommon), 4=State/provincial capital
		if capital ~="" then
			if admin_level == 2 then
				node:AttributeNumeric("capital",2)
			end
			if admin_level == 4 then
				node:AttributeNumeric("capital",4)
			end
		end
		-- The value number from 0 through 9, where 0 is the large end of the scale (eg New York City).
		-- All places other than large cities will have a scalerank of null.
		if place == "village" then
			node:AttributeNumeric("scalerank",3)
		end
		if place == "town" then
			node:AttributeNumeric("scalerank",5)
		end
		if place == "suburb" then
			node:AttributeNumeric("scalerank",7)
		end
		if place == "city" then
			node:AttributeNumeric("scalerank",9)
		end
		-- Therefore to reduce the label density to 4 labels per tile, you can add the filter [localrank=1].
		node:AttributeNumeric("localrank",1)
		-- The ldir field can be used as a hint for label offset directions at lower zoom levels.
		node:Attribute("ldir","N")
	end
	-- POI Attributes
	-- "localrank","maki","name","name_de","name_en","name_es","name_fr","name_ru",
	-- "name_zh","osm_id","ref","scalerank","type","network"

	if not amenity_exclude[amenity] then
		Statistics(node, "amenity")
		node:Layer("poi_label", false)
		
		-- OSM uses "_" while the style uses "-" for icons 
		local amenity_rep = string.gsub(amenity, "_", "-")
		node:AttributeNumeric("scalerank",3)

		-- set icon & name, where icon & name is available
		-- set icon only if no name is available, this reduces file size
		if icons[amenity_rep] and name~= "" then
			node:Attribute("maki", amenity_rep)
			node:Attribute("name", name)
		elseif icons[amenity_rep] then
			node:Attribute("maki", amenity_rep)
		elseif name~= "" then
			node:Attribute("maki", "marker")
			node:Attribute("name", name)
		else
			-- if you want to display default markers
			-- for pois without icon & name
			--node:Attribute("maki", "marker")
		end

		if aeroway ~= "" then
			node:Attribute("maki","airport")
		end
	end
	if not shop_exclude[shop] then
		Statistics(node, "shop")
		node:Layer("poi_label", false)

		local shop_rep = string.gsub(shop, "_", "-")
		node:AttributeNumeric("scalerank",3)

		node:Attribute("name", name)
		if icons[shop_rep] then
			node:Attribute("maki", shop_rep)
		else
			node:Attribute("maki", "marker")
		end
		--if shop == "alcohol" then
		--	io.write(debug_print and "-Amenity-" or "")
		--	node:Attribute("maki","alcohol-shop")
		--	node:AttributeNumeric("scalerank",3)
		--end
	end
end

function way_function(way)
	-- layers:main
	local landuse = way:Find("landuse")
	local waterway = way:Find("waterway")
	local water = way:Find("water")
	local natural = way:Find("natural")
	local aeroway = way:Find("aeorway")
	local barrier_line = way:Find("barrier_line")
	local building = way:Find("building")
	local landuse_overlay = way:Find("landuse_overlay")
	local road = way:Find("highway")
	local admin = way:Find("admin_level")

	-- helpers
	local bridge = way:Find("bridge");
	local railway = way:Find("railway");
	local leisure = way:Find("leisure");
	local name = way:Find("name");
	local name_en = way:Find("name:en");

--"agriculture","farmland","wood","forest","park","golf_course","farm","grass","meadow","scrub",
--"heath","allotments","camp_site","plant_nursery","cemetery","sports_centre","grassland",
--"farmyard","parking","school","rock","scree","village_green","orchard","pitch","soccer",
--"athletics","christian","playground","recreation_ground"
	if not landuse_exclude[landuse] then
		Statistics(node, "landuse")

		way:Layer("landuse", true)
		way:Attribute("class",landuse)
		if landuse=="allotments" then way:Attribute("class","park") end
		if leisure~="" then
			io.write(debug_print and "-Leisure" or "")
			way:Attribute("class","park")
		end
	end
	if waterway~="" then
		Statistics(node, "water label")

		way:Layer("waterway_label", true)
		-- 'river','canal','stream','stream_intermittent','drain','ditch'
		way:Attribute("class",waterway)
		way:Attribute("type",waterway)
		way:Attribute("name",name)
	end
	if waterway~="" and waterway~="riverbank" then
		Statistics(node, "riverbank")

		way:Layer("waterway", false)
		-- 'river','canal','stream','stream_intermittent','drain','ditch'
		way:Attribute("class",waterway)
		way:Attribute("type",waterway)
		way:Attribute("name",name)
	end
	if waterway =="riverbank" then

		way:Layer("water", true)
		way:Attribute("class","river")
		way:Attribute("type","river")
	end
	if not natural_exclude[natural] then
		-- The class and type fields match those in the #waterway layer.
		-- 'river','canal','stream','stream_intermittent','drain','ditch'
		Statistics(node, "water")

		way:Layer("water", true)
		way:Attribute("class",waterway)
		way:Attribute("type",waterway)
		way:Attribute("name",name)
	end

    ----------------
	-- road layer --
	----------------
	if not road_exclude[road] then
		if bridge~="" then
    		way:Layer("road_bridge", false)
    		way:Layer("bridge", false)
  		else
    		way:Layer("road", false)
  		end
		way:Attribute("class",road)
		way:Attribute("type",road)
		way:Attribute("layer",road)
		if road=="residential" then way:Attribute("class","street") end
		if road=="footway" or road=="cycleway" then
			way:Attribute("class","path")
		end
		if road=="primary" 
			or road=="secondary" 
			or road=="tertiary"
			then way:Attribute("class","main") end
		if road=="primary_link" or road=="secondary_link" then
			way:Attribute("class","street")
		end
		if bridge~="" then
			way:Layer("road_bridge", false)
			way:Layer("bridge", false)
		end
	end
	if not road_exclude[road] then
		-- "class","len","localrank","osm_id","ref","reflen","shield","name","name_de","name_en","name_es","name_fr","name_ru","name_zh"
		way:Layer("road_label", false)
		way:Attribute("name",name)
		if way:Find("ref") ~="" then
			way:Attribute("ref",way:Find("ref"))
			way:AttributeNumeric("reflen", string.len(way:Find("ref")))
		end
		way:AttributeNumeric("osm_id",tonumber(way:Id()))
		way:Attribute("shield","default")
		way:AttributeNumeric("len", 100)
		way:AttributeNumeric("localrank", 3)
		--"class","len","localrank","name","name_de","name_en","name_es","name_fr","name_ru","name_zh","osm_id","ref","reflen","shield"
		--way:AttributeBoolean("oneway": "false")
		if road=="residential" then way:Attribute("class","street") end
		if road=="footway" or road=="cycleway" then
			way:Attribute("class","path")
		end
		if road=="primary" or road=="secondary" or road=="tertiary" then 
				way:Attribute("class","main")
		end
		if road=="primary_link" or road=="secondary_link" then
			way:Attribute("class","street")
		end
    end
	--if road~="" then
	--	way:Attribute("class",road)
	--	way:Layer("road_label", false)
	--	way:Attribute("name",name)
	--	way:Attribute("ref",way:Find("ref"))
	--	way:Attribute("shield","default")
	--end
	if not railway_exclude[railway] then
		way:Layer("road", false)
		if railway == 'rail' then
			way:Attribute("class", 'major_rail')
		end
	end

	if not landuse_overlay_exclude[landuse_overlay] then
		way:Attribute("class",way:Find("construction"))
		way:Layer("landuse_overlay", false)
	end

	if not aeroway_exclude[aeroway] then
		way:Layer("aeroway", false)
		way:Attribute("type",aeroway)
	end
	if not barrier_line_exclude[barrier_line] then
		way:Layer("barrier_line", false)
		way:Attribute("class",barrier_line)

	end
	if not building_exclude[building] then
		way:Layer("building", true)
	end
end
