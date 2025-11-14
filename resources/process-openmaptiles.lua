-- Data processing based on openmaptiles.org schema
-- https://openmaptiles.org/schema/
-- Copyright (c) 2016, KlokanTech.com & OpenMapTiles contributors.
-- Used under CC-BY 4.0

--------
-- Alter these lines to control which languages are written for place/streetnames
--
-- Preferred language can be (for example) "en" for English, "de" for German, or nil to use OSM's name tag:
preferred_language = nil
-- This is written into the following vector tile attribute (usually "name:latin"):
preferred_language_attribute = "name:latin"
-- If OSM's name tag differs, then write it into this attribute (usually "name_int"):
default_language_attribute = "name_int"
-- Also write these languages if they differ - for example, { "de", "fr" }
additional_languages = { }
--------

-- Enter/exit Tilemaker
function init_function(name,is_first)
end
function exit_function()
end

-- Implement Sets in tables
function Set(list)
	local set = {}
	for _, l in ipairs(list) do set[l] = true end
	return set
end

-- Meters per pixel if tile is 256x256
ZRES0 = 156542.97
ZRES1  = 78271.48
ZRES2  = 39135.74
ZRES3  = 19567.87
ZRES4  = 9783.94
ZRES5  = 4891.97
ZRES6  = 2445.98
ZRES7  = 1222.99
ZRES8  = 611.5
ZRES9  = 305.7
ZRES10 = 152.9
ZRES11 = 76.4
ZRES12 = 38.2
ZRES13 = 19.1

-- The height of one floor, in meters
BUILDING_FLOOR_HEIGHT = 3.66
-- Used to express that a feature should not end up the vector tiles
INVALID_ZOOM = 99

-- Process node/way tags
aerodromeValues = Set { "international", "public", "regional", "military", "private" }
unpavedValues = Set { "unpaved", "artificial_turf", "clay", "compacted", "crushed_limestone", "dirt", "dirt/sand", "earth", "fine_gravel", "grass", "grass_paver", "gravel", "gravel_turf", "ground", "ice", "mud", "pebblestone", "rock", "rocky", "rocks", "salt", "sand", "shells", "snow", "soil", "stepping_stones", "stone", "woodchips" }
pavedValues = Set { "paved", "acrylic", "asphalt", "brick", "bricks", "cement", "chipseal", "cobblestone", "concrete", "concrete:lanes", "concrete:plates", "granite", "interlock", "metal", "metal_grid", "paving_stones", "plastic", "rubber", "sett", "tartan", "unhewn_cobblestone", "wood", "grade1" }

-- Process node tags

node_keys = { "addr:housenumber","aerialway","aeroway","amenity","barrier","highway","historic","leisure","natural","office","place","railway","shop","sport","tourism","waterway" }

-- Get admin level which the place node is capital of.
-- Returns nil in case of invalid capital and for places which are not capitals.
function capitalLevel(capital)
	local capital_al = tonumber(capital) or 0
	if capital == "yes" then
		capital_al = 2
	end
	if capital_al == 0 then
		return nil
	end
	return capital_al
end

-- Calculate rank for place nodes
-- place: value of place=*
-- popuplation: population as number
-- capital_al: result of capitalLevel()
function calcRank(place, population, capital_al)
	local rank = 0
	if capital_al and capital_al >= 2 and capital_al <= 4 then
		rank = capital_al
		if population > 3 * 10^6 then
			rank = rank - 2
		elseif population > 1 * 10^6 then
			rank = rank - 1
		elseif population < 100000 then
			rank = rank + 2
		elseif population < 50000 then
			rank = rank + 3
		end
		-- Safety measure to avoid place=village/farm/... appear early (as important capital) because a mapper added capital=yes/2/3/4
		if place ~= "city" then
			rank = rank + 3
			-- Decrease rank further if it is not even a town.
			if place ~= "town" then
				rank = rank + 2
			end
		end
		return rank
	end
	if place ~= "city" and place ~= "town" then
		return nil
	end
	if population > 3 * 10^6 then
		return 1
	elseif population > 1 * 10^6 then
		return 2
	elseif population > 500000 then
		return 3
	elseif population > 200000 then
		return 4
	elseif population > 100000 then
		return 5
	elseif population > 75000 then
		return 6
	elseif population > 50000 then
		return 7
	elseif population > 25000 then
		return 8
	elseif population > 10000 then
		return 9
	end
	return 10
end


function node_function()
	-- Write 'aerodrome_label'
	local aeroway = Find("aeroway")
	if aeroway == "aerodrome" then
		Layer("aerodrome_label", false)
		SetNameAttributes()
		Attribute("iata", Find("iata"))
		SetEleAttributes()
		Attribute("icao", Find("icao"))

		local aerodrome_value = Find("aerodrome")
		local class
		if aerodromeValues[aerodrome_value] then class = aerodrome_value else class = "other" end
		Attribute("class", class)
	end
	-- Write 'housenumber'
	local housenumber = Find("addr:housenumber")
	if housenumber~="" then
		Layer("housenumber", false)
		Attribute("housenumber", housenumber)
	end

	-- Write 'place'
	-- note that OpenMapTiles has a rank for countries (1-3), states (1-6) and cities (1-10+);
	--   we could potentially approximate it for cities based on the population tag
	local place = Find("place")
	if place ~= "" then
		local mz = 13
		local pop = tonumber(Find("population")) or 0
		local capital = capitalLevel(Find("capital"))
		local rank = calcRank(place, pop, capital)

		if     place == "continent"     then mz=0
		elseif place == "country"       then
			if     pop>50000000 then rank=1; mz=1
			elseif pop>20000000 then rank=2; mz=2
			else                     rank=3; mz=3 end
		elseif place == "state"         then mz=4
		elseif place == "province"         then mz=5
		elseif place == "city"          then mz=5
		elseif place == "town" and pop>8000 then mz=7
		elseif place == "town"          then mz=8
		elseif place == "village" and pop>2000 then mz=9
		elseif place == "village"       then mz=10
		elseif place == "borough"       then mz=10
		elseif place == "suburb"        then mz=11
		elseif place == "quarter"       then mz=12
		elseif place == "hamlet"        then mz=12
		elseif place == "neighbourhood" then mz=13
		elseif place == "isolated_dwelling" then mz=13
		elseif place == "locality"      then mz=13
		elseif place == "island"      then mz=12
		end

		Layer("place", false)
		Attribute("class", place)
		MinZoom(mz)
		if rank then AttributeInteger("rank", rank) end
		if capital then AttributeInteger("capital", capital) end
		if place=="country" then
			local iso_a2 = Find("ISO3166-1:alpha2")
			while iso_a2 == "" do
				local rel, role = NextRelation()
				if not rel then break end
				if role == 'label' then
					iso_a2 = FindInRelation("ISO3166-1:alpha2")
				end
			end
			Attribute("iso_a2", iso_a2)
		end
		SetNameAttributes()
		return
	end

	-- Write 'poi'
	local rank, class, subclass = GetPOIRank()
	if rank then WritePOI(class,subclass,rank) end

	-- Write 'mountain_peak' and 'water_name'
	local natural = Find("natural")
	if natural == "peak" or natural == "volcano" then
		Layer("mountain_peak", false)
		SetEleAttributes()
		AttributeInteger("rank", 1)
		Attribute("class", natural)
		SetNameAttributes()
		return
	end
	if natural == "bay" then
		Layer("water_name", false)
		SetNameAttributes()
		return
	end
end

-- Process way tags

z4RoadValues = Set { "motorway" }
z5RoadValues = Set { "trunk" }
z7RoadValues = Set { "primary" }
z9RoadValues = Set { "secondary", "motorway_link", "trunk_link" }
z10RoadValues = Set { "primary_link", "secondary_link" }
z11RoadValues = Set { "tertiary", "tertiary_link", "busway", "bus_guideway" }
-- On zoom 12, various road classes are merged into "minor"
z12MinorRoadValues = Set { "unclassified", "residential", "road", "living_street" }
z12OtherRoadValues = Set { "raceway" }
z13RoadValues     = Set { "track", "service" }
manMadeRoadValues = Set { "pier", "bridge" }
pathValues      = Set { "footway", "cycleway", "bridleway", "path", "steps", "pedestrian", "platform" }
linkValues      = Set { "motorway_link", "trunk_link", "primary_link", "secondary_link", "tertiary_link" }
railwayClasses  = { rail="rail", narrow_gauge="rail", preserved="rail", funicular="rail", subway="transit", light_rail="transit", monorail="transit", tram="transit" }

aerowayBuildings= Set { "terminal", "gate", "tower" }
landuseKeys     = Set { "school", "university", "kindergarten", "college", "library", "hospital",
                        "railway", "cemetery", "military", "residential", "commercial", "industrial",
                        "retail", "stadium", "pitch", "playground", "theme_park", "bus_station", "zoo" }
landcoverKeys   = { wood="wood", forest="wood",
                    wetland="wetland",
                    beach="sand", sand="sand", dune="sand",
                    farmland="farmland", farm="farmland", orchard="farmland", vineyard="farmland", plant_nursery="farmland",
                    glacier="ice", ice_shelf="ice",
                    bare_rock="rock", scree="rock",
                    fell="grass", grassland="grass", grass="grass", heath="grass", meadow="grass", allotments="grass", park="grass", village_green="grass", recreation_ground="grass", scrub="grass", shrubbery="grass", tundra="grass", garden="grass", golf_course="grass", park="grass" }

-- POI key/value pairs: based on https://github.com/openmaptiles/openmaptiles/blob/master/layers/poi/mapping.yaml
poiTags         = { aerialway = Set { "station" },
					amenity = Set { "arts_centre", "bank", "bar", "bbq", "bicycle_parking", "bicycle_rental", "biergarten", "bus_station", "cafe", "cinema", "clinic", "college", "community_centre", "courthouse", "dentist", "doctors", "embassy", "fast_food", "ferry_terminal", "fire_station", "food_court", "fuel", "grave_yard", "hospital", "ice_cream", "kindergarten", "library", "marketplace", "motorcycle_parking", "nightclub", "nursing_home", "parking", "pharmacy", "place_of_worship", "police", "post_box", "post_office", "prison", "pub", "public_building", "recycling", "restaurant", "school", "shelter", "swimming_pool", "taxi", "telephone", "theatre", "toilets", "townhall", "university", "veterinary", "waste_basket" },
					barrier = Set { "bollard", "border_control", "cycle_barrier", "gate", "lift_gate", "sally_port", "stile", "toll_booth" },
					building = Set { "dormitory" },
					highway = Set { "bus_stop" },
					historic = Set { "monument", "castle", "ruins" },
					landuse = Set { "basin", "brownfield", "cemetery", "reservoir", "winter_sports" },
					leisure = Set { "dog_park", "escape_game", "garden", "golf_course", "ice_rink", "hackerspace", "marina", "miniature_golf", "park", "pitch", "playground", "sports_centre", "stadium", "swimming_area", "swimming_pool", "water_park" },
					railway = Set { "halt", "station", "subway_entrance", "train_station_entrance", "tram_stop" },
					shop = Set { "accessories", "alcohol", "antiques", "art", "bag", "bakery", "beauty", "bed", "beverages", "bicycle", "books", "boutique", "butcher", "camera", "car", "car_repair", "carpet", "charity", "chemist", "chocolate", "clothes", "coffee", "computer", "confectionery", "convenience", "copyshop", "cosmetics", "deli", "delicatessen", "department_store", "doityourself", "dry_cleaning", "electronics", "erotic", "fabric", "florist", "frozen_food", "furniture", "garden_centre", "general", "gift", "greengrocer", "hairdresser", "hardware", "hearing_aids", "hifi", "ice_cream", "interior_decoration", "jewelry", "kiosk", "lamps", "laundry", "mall", "massage", "mobile_phone", "motorcycle", "music", "musical_instrument", "newsagent", "optician", "outdoor", "perfume", "perfumery", "pet", "photo", "second_hand", "shoes", "sports", "stationery", "supermarket", "tailor", "tattoo", "ticket", "tobacco", "toys", "travel_agency", "video", "video_games", "watches", "weapons", "wholesale", "wine" },
					sport = Set { "american_football", "archery", "athletics", "australian_football", "badminton", "baseball", "basketball", "beachvolleyball", "billiards", "bmx", "boules", "bowls", "boxing", "canadian_football", "canoe", "chess", "climbing", "climbing_adventure", "cricket", "cricket_nets", "croquet", "curling", "cycling", "disc_golf", "diving", "dog_racing", "equestrian", "fatsal", "field_hockey", "free_flying", "gaelic_games", "golf", "gymnastics", "handball", "hockey", "horse_racing", "horseshoes", "ice_hockey", "ice_stock", "judo", "karting", "korfball", "long_jump", "model_aerodrome", "motocross", "motor", "multi", "netball", "orienteering", "paddle_tennis", "paintball", "paragliding", "pelota", "racquet", "rc_car", "rowing", "rugby", "rugby_league", "rugby_union", "running", "sailing", "scuba_diving", "shooting", "shooting_range", "skateboard", "skating", "skiing", "soccer", "surfing", "swimming", "table_soccer", "table_tennis", "team_handball", "tennis", "toboggan", "volleyball", "water_ski", "yoga" },
					tourism = Set { "alpine_hut", "aquarium", "artwork", "attraction", "bed_and_breakfast", "camp_site", "caravan_site", "chalet", "gallery", "guest_house", "hostel", "hotel", "information", "motel", "museum", "picnic_site", "theme_park", "viewpoint", "zoo" },
					waterway = Set { "dock" } }

-- POI "class" values: based on https://github.com/openmaptiles/openmaptiles/blob/master/layers/poi/poi.yaml
poiClasses      = { townhall="town_hall", public_building="town_hall", courthouse="town_hall", community_centre="town_hall",
					golf="golf", golf_course="golf", miniature_golf="golf",
					fast_food="fast_food", food_court="fast_food",
					park="park", bbq="park",
					bus_stop="bus", bus_station="bus",
					subway_entrance="entrance", train_station_entrance="entrance",
					camp_site="campsite", caravan_site="campsite",
					laundry="laundry", dry_cleaning="laundry",
					supermarket="grocery", deli="grocery", delicatessen="grocery", department_store="grocery", greengrocer="grocery", marketplace="grocery",
					books="library", library="library",
					university="college", college="college",
					hotel="lodging", motel="lodging", bed_and_breakfast="lodging", guest_house="lodging", hostel="lodging", chalet="lodging", alpine_hut="lodging", dormitory="lodging",
					chocolate="ice_cream", confectionery="ice_cream",
					post_box="post",  post_office="post",
					cafe="cafe",
					school="school",  kindergarten="school",
					alcohol="alcohol_shop",  beverages="alcohol_shop",  wine="alcohol_shop",
					bar="bar", nightclub="bar",
					marina="harbor", dock="harbor",
					car="car", car_repair="car", taxi="car",
					hospital="hospital", nursing_home="hospital",  clinic="hospital",
					grave_yard="cemetery", cemetery="cemetery",
					attraction="attraction", viewpoint="attraction",
					biergarten="beer", pub="beer",
					music="music", musical_instrument="music",
					american_football="stadium", stadium="stadium", soccer="stadium",
					art="art_gallery", artwork="art_gallery", gallery="art_gallery", arts_centre="art_gallery",
					bag="clothing_store", clothes="clothing_store",
					swimming_area="swimming", swimming="swimming",
					castle="castle", ruins="castle" }
-- POI classes where class is the matching value and subclass is the value of a separate key
poiSubClasses = { information="information", place_of_worship="religion", pitch="sport" }
poiClassRanks   = { hospital=1, railway=2, bus=3, attraction=4, harbor=5, college=6,
					school=7, stadium=8, zoo=9, town_hall=10, campsite=11, cemetery=12,
					park=13, library=14, police=15, post=16, golf=17, shop=18, grocery=19,
					fast_food=20, clothing_store=21, bar=22 }
waterClasses    = Set { "river", "riverbank", "stream", "canal", "drain", "ditch", "dock" }
waterwayClasses = Set { "stream", "river", "canal", "drain", "ditch" }

-- Scan relations for use in ways

function relation_scan_function()
	if Find("type")=="boundary" and Find("boundary")=="administrative" then
		Accept()
	end
end

function write_to_transportation_layer(minzoom, highway_class, subclass, ramp, service, is_rail, is_road, is_area)
	Layer("transportation", is_area)
	SetZOrder()
	Attribute("class", highway_class)
	if subclass and subclass ~= "" then
		Attribute("subclass", subclass)
	end
	local accessMinzoom = 9
	local layer = tonumber(Find("layer")) or 0
	AttributeInteger("layer", math.floor(layer), accessMinzoom)
	SetBrunnelAttributes()
	-- We do not write any other attributes for areas.
	if is_area then
		SetMinZoomByAreaWithLimit(minzoom)
		return
	end
	MinZoom(minzoom)
	if ramp then AttributeInteger("ramp",1) end

	-- Service
	if (is_rail or highway_class == "service") and (service and service ~="") then Attribute("service", service) end

	if is_road then
		local oneway = Find("oneway")
		if oneway == "yes" or oneway == "1" then
			AttributeInteger("oneway",1)
		end
		if oneway == "-1" then
			-- **** TODO
		end
		local surfaceMinzoom = 12
		local surface = GetSurface()
		if surface ~= "" then Attribute("surface", surface, surfaceMinzoom) end
		if Holds("access") then Attribute("access", Find("access"), accessMinzoom) end
		if Holds("bicycle") then Attribute("bicycle", Find("bicycle"), accessMinzoom) end
		if Holds("foot") then Attribute("foot", Find("foot"), accessMinzoom) end
		if Holds("horse") then Attribute("horse", Find("horse"), accessMinzoom) end
		AttributeBoolean("toll", Find("toll") == "yes", accessMinzoom)
		if Find("expressway") == "yes" then AttributeBoolean("expressway", true, 7) end
		if Holds("mtb_scale") then Attribute("mtb_scale", Find("mtb:scale"), 10) end
	end
end

function GetSurface()
	local surface = split(Find("surface"), ";")
	-- prioritize unpaved
	for _, surfaceEntry in ipairs(surface) do
		if unpavedValues[surfaceEntry] then return "unpaved" end
	end
	for _, surfaceEntry in ipairs(surface) do
		if pavedValues[surfaceEntry] then return "paved" end
	end
	return ""
end

-- Process way tags

function way_function()
	local route    = Find("route")
	local highway  = Find("highway")
	local waterway = Find("waterway")
	local water    = Find("water")
	local building = Find("building")
	local natural  = Find("natural")
	local historic = Find("historic")
	local landuse  = Find("landuse")
	local leisure  = Find("leisure")
	local amenity  = Find("amenity")
	local aeroway  = Find("aeroway")
	local railway  = Find("railway")
	local service  = Find("service")
	local sport    = Find("sport")
	local shop     = Find("shop")
	local tourism  = Find("tourism")
	local man_made = Find("man_made")
	local boundary = Find("boundary")
	local aerialway  = Find("aerialway")
	local public_transport  = Find("public_transport")
	local place = Find("place")
	local is_closed = IsClosed()
	local housenumber = Find("addr:housenumber")
	local write_name = false
	local construction = Find("construction")
	local is_highway_area = highway~="" and Find("area")=="yes" and is_closed

	-- Miscellaneous preprocessing
	if Find("disused") == "yes" then return end
	if boundary~="" and Find("protection_title")=="National Forest" and Find("operator")=="United States Forest Service" then return end
	if highway == "proposed" then return end
	if aerowayBuildings[aeroway] then building="yes"; aeroway="" end
	if landuse == "field" then landuse = "farmland" end
	if landuse == "meadow" and Find("meadow")=="agricultural" then landuse="farmland" end

	if place == "island" then
		LayerAsCentroid("place")
		Attribute("class", place)
		MinZoom(10)
		local pop = tonumber(Find("population")) or 0
		local capital = capitalLevel(Find("capital"))
		local rank = calcRank(place, pop, nil)
		if rank then AttributeInteger("rank", rank) end
		SetNameAttributes()
	end

	-- Boundaries within relations
	-- note that we process administrative boundaries as properties on ways, rather than as single relation geometries,
	--  because otherwise we get multiple renderings where boundaries are coterminous
	local admin_level = 11
	local isBoundary = false
	while true do
		local rel = NextRelation()
		if not rel then break end
		isBoundary = true
		admin_level = math.min(admin_level, tonumber(FindInRelation("admin_level")) or 11)
	end

	-- Boundaries in ways
	if boundary=="administrative" then
		admin_level = math.min(admin_level, tonumber(Find("admin_level")) or 11)
		isBoundary = true
	end

	-- Administrative boundaries
	-- https://openmaptiles.org/schema/#boundary
	if isBoundary and not (Find("maritime")=="yes") then
		local mz = 0
		if     admin_level>=3 and admin_level<5 then mz=3
		elseif admin_level>=5 and admin_level<7 then mz=8
		elseif admin_level==7 then mz=10
		elseif admin_level>=8 then mz=12
		end

		Layer("boundary",false)
		AttributeInteger("admin_level", admin_level)
		MinZoom(mz)
		-- disputed status (0 or 1). some styles need to have the 0 to show it.
		local disputed = Find("disputed")
		if disputed=="yes" then
			AttributeInteger("disputed", 1)
		else
			AttributeInteger("disputed", 0)
		end
	end

	-- Aerialways ('transportation' and 'transportation_name')
	if aerialway ~= "" then
		write_to_transportation_layer(12, "aerialway", aerialway, false, nil, false, false, is_closed)
		if HasNames() then
			Layer("transportation_name", false)
			MinZoom(12)
			SetNameAttributes()
			Attribute("class", "aerialway")
			Attribute("subclass", aerialway)
		end
	end

	-- Roads ('transportation' and 'transportation_name')
	if highway ~= "" or public_transport == "platform" then
		local access = Find("access")
		local surface = Find("surface")

		local h = highway
		local is_road = true
		if h == "" then
			h = public_transport
			is_road = false
		end
		local subclass = nil
		local under_construction = false
		if highway == "construction" and construction ~= "" then
			h = construction
			under_construction = true
		end
		local minzoom = INVALID_ZOOM
		if z4RoadValues[h]           then minzoom = 4
		elseif z5RoadValues[h]       then minzoom = 5
		elseif z7RoadValues[h]       then minzoom = 7
		elseif z9RoadValues[h]       then minzoom = 9
		elseif z10RoadValues[h]      then minzoom = 10
		elseif z11RoadValues[h]      then minzoom = 11
		elseif z12MinorRoadValues[h] then
			minzoom = 12
			subclass = h
			h = "minor"
		elseif z12OtherRoadValues[h] then minzoom = 12
		elseif z13RoadValues[h]      then minzoom = 13
		elseif pathValues[h]         then
			minzoom = 14
			subclass = h
			h = "path"
		end

		-- Links (ramp)
		local ramp=false
		if linkValues[h] then
			splitHighway = split(highway, "_")
			highway = splitHighway[1]; h = highway
			ramp = true
		end

		-- Construction
		if under_construction then
			h = h .. "_construction"
		end

		-- Drop underground platforms
		local layer = Find("layer")
		local layerNumeric = tonumber(layer)
		if not is_road and layerNumeric ~= nil and layerNumeric < 0 then
			minzoom = INVALID_ZOOM
		end

		-- Drop all areas except infrastructure for pedestrians handled above
		if is_highway_area and h ~= "path" then
			minzoom = INVALID_ZOOM
		end

		-- Write to layer
		if minzoom <= 14 then
			write_to_transportation_layer(minzoom, h, subclass, ramp, service, false, is_road, is_highway_area)

			-- Write names
			if not is_closed and (HasNames() or Holds("ref")) then
				if h == "motorway" then
					minzoom = 7
				elseif h == "trunk" then
					minzoom = 8
				elseif h == "primary" then
					minzoom = 10
				elseif h == "secondary" then
					minzoom = 11
				elseif h == "minor" or h == "track" or h == "tertiary" then
					minzoom = 13
				else
					minzoom = 14
				end
				Layer("transportation_name", false)
				MinZoom(minzoom)
				SetNameAttributes()
				Attribute("class",h)
				Attribute("network","road") -- **** could also be us-interstate, us-highway, us-state
				if subclass then Attribute("subclass", highway) end
				local ref = Find("ref")
				if ref~="" then
					Attribute("ref",ref)
					AttributeInteger("ref_length",ref:len())
				end
			end
		end
	end

	-- Railways ('transportation' and 'transportation_name')
	if railway~="" then
		local class = railwayClasses[railway]
		if class then
			local minzoom = 14
			local usage = Find("usage")
			if railway == "rail" and service == "" then
				if usage == "main" then
					minzoom = 8
				else
					minzoom = 10
				end
			elseif railway == "narrow_gauge" and service == "" then
				minzoom = 10
			elseif railway == "light_rail" and service == "" then
				minzoom = 11
			end
			write_to_transportation_layer(minzoom, class, railway, false, service, true, false, is_closed)

			if HasNames() then
				Layer("transportation_name", false)
				SetNameAttributes()
				MinZoom(14)
				Attribute("class", class)
			end
		end
	end

	-- Pier
	if manMadeRoadValues[man_made] then
		write_to_transportation_layer(13, man_made, nil, false, nil, false, false, is_closed)
	end

	-- 'Ferry'
	if route=="ferry" then
		write_to_transportation_layer(9, "ferry", nil, false, nil, false, false, is_closed)

		if HasNames() then
			Layer("transportation_name", false)
			SetNameAttributes()
			MinZoom(12)
			Attribute("class", "ferry")
		end
	end

	-- 'Aeroway'
	if aeroway~="" then
		Layer("aeroway", is_closed)
		Attribute("class",aeroway)
		Attribute("ref",Find("ref"))
		write_name = true
	end

	-- 'aerodrome_label'
	if aeroway=="aerodrome" then
		LayerAsCentroid("aerodrome_label")
		SetNameAttributes()
		Attribute("iata", Find("iata"))
		SetEleAttributes()
		Attribute("icao", Find("icao"))

		local aerodrome = Find(aeroway)
		local class
		if aerodromeValues[aerodrome] then class = aerodrome else class = "other" end
		Attribute("class", class)
	end

	-- Set 'waterway' and associated
	if waterwayClasses[waterway] and not is_closed then
		if waterway == "river" and Holds("name") then
			Layer("waterway", false)
		else
			Layer("waterway_detail", false)
		end
		if Find("intermittent")=="yes" then AttributeInteger("intermittent", 1) else AttributeInteger("intermittent", 0) end
		Attribute("class", waterway)
		SetNameAttributes()
		SetBrunnelAttributes()
	elseif waterway == "boatyard"  then Layer("landuse", is_closed); Attribute("class", "industrial"); MinZoom(12)
	elseif waterway == "dam"       then Layer("building",is_closed)
	elseif waterway == "fuel"      then Layer("landuse", is_closed); Attribute("class", "industrial"); MinZoom(14)
	end
	-- Set names on rivers
	if waterwayClasses[waterway] and not is_closed then
		if waterway == "river" and Holds("name") then
			Layer("water_name", false)
		else
			Layer("water_name_detail", false)
			MinZoom(14)
		end
		Attribute("class", waterway)
		SetNameAttributes()
	end

	-- Set 'building' and associated
	if building~="" then
		Layer("building", true)
		SetBuildingHeightAttributes()
		SetMinZoomByArea()
	end

	-- Set 'housenumber'
	if housenumber~="" then
		LayerAsCentroid("housenumber")
		Attribute("housenumber", housenumber)
	end

	-- Set 'water'
	if natural=="water" or leisure=="swimming_pool" or landuse=="reservoir" or landuse=="basin" or waterClasses[waterway] then
		if Find("covered")=="yes" or not is_closed then return end
		local class="lake"; if waterway~="" then class="river" end
		if class=="lake" and Find("wikidata")=="Q192770" then return end
		Layer("water",true)
		SetMinZoomByArea()
		Attribute("class",class)

		if Find("intermittent")=="yes" then Attribute("intermittent",1) end
		-- we only want to show the names of actual lakes not every man-made basin that probably doesn't even have a name other than "basin"
		-- examples for which we don't want to show a name:
		--  https://www.openstreetmap.org/way/25958687
		--  https://www.openstreetmap.org/way/27201902
		--  https://www.openstreetmap.org/way/25309134
		--  https://www.openstreetmap.org/way/24579306
		if Holds("name") and natural=="water" and water ~= "basin" and water ~= "wastewater" then
			LayerAsCentroid("water_name_detail")
			SetNameAttributes()
			SetMinZoomByArea()
			Attribute("class", class)
		end

		return -- in case we get any landuse processing
	end

	-- Set 'landcover' (from landuse, natural, leisure)
	local l = landuse
	if l=="" then l=natural end
	if l=="" then l=leisure end
	if landcoverKeys[l] then
		Layer("landcover", true)
		SetMinZoomByArea()
		Attribute("class", landcoverKeys[l])
		if l=="wetland" then Attribute("subclass", Find("wetland"))
		else Attribute("subclass", l) end
		write_name = true

	-- Set 'landuse'
	else
		if l=="" then l=amenity end
		if l=="" then l=tourism end
		if landuseKeys[l] then
			Layer("landuse", true)
			Attribute("class", l)
			if l=="residential" then
				if Area()<ZRES8^2 then MinZoom(8)
				else SetMinZoomByArea() end
			else MinZoom(11) end
			write_name = true
		end
	end

	-- Parks
	-- **** name?
	if     boundary=="national_park" then Layer("park",true); Attribute("class",boundary); SetNameAttributes()
	elseif leisure=="nature_reserve" then Layer("park",true); Attribute("class",leisure ); SetNameAttributes() end

	-- POIs ('poi' and 'poi_detail')
	local rank, class, subclass = GetPOIRank()
	if rank then WritePOI(class,subclass,rank); return end

	-- Catch-all
	if (building~="" or write_name) and Holds("name") then
		LayerAsCentroid("poi_detail")
		SetNameAttributes()
		if write_name then rank=6 else rank=25 end
		AttributeInteger("rank", rank)
	end
end

-- Remap coastlines
function attribute_function(attr,layer)
	if attr["featurecla"]=="Glaciated areas" then
		return { subclass="glacier" }
	elseif attr["featurecla"]=="Antarctic Ice Shelf" then
		return { subclass="ice_shelf" }
	elseif attr["featurecla"]=="Urban area" then
		return { class="residential" }
	elseif layer=="ocean" then
		return { class="ocean" }
	else
		return attr
	end
end

-- ==========================================================
-- Common functions

-- Write a way centroid to POI layer
function WritePOI(class,subclass,rank)
	local layer = "poi"
	if rank>4 then layer="poi_detail" end
	LayerAsCentroid(layer)
	SetNameAttributes()
	AttributeInteger("rank", rank)
	Attribute("class", class)
	Attribute("subclass", subclass)
	-- layer defaults to 0
	local layer = tonumber(Find("layer")) or 0
	AttributeInteger("layer", math.floor(layer))
	-- indoor defaults to false
	AttributeBoolean("indoor", (Find("indoor") == "yes"))
	-- level has no default
	local level = tonumber(Find("level"))
	if level then
		AttributeInteger("level", math.floor(level))
	end
end

-- Check if there are name tags on the object
function HasNames()
	if Holds("name") then return true end
	local iname
	local main_written = name
	if preferred_language and Holds("name:"..preferred_language) then return true end
	-- then set any additional languages
	for i,lang in ipairs(additional_languages) do
		if Holds("name:"..lang) then return true end
	end
	return false
end

-- Set name attributes on any object
function SetNameAttributes()
	local name = Find("name"), iname
	local main_written = name
	-- if we have a preferred language, then write that (if available), and additionally write the base name tag
	if preferred_language and Holds("name:"..preferred_language) then
		iname = Find("name:"..preferred_language)
		Attribute(preferred_language_attribute, iname)
		if iname~=name and default_language_attribute then
			Attribute(default_language_attribute, name)
		else main_written = iname end
	elseif name ~= "" then
		Attribute(preferred_language_attribute, name)
	end
	-- then set any additional languages
	for i,lang in ipairs(additional_languages) do
		iname = Find("name:"..lang)
		if iname=="" then iname=name end
		if iname~=main_written and iname ~= "" then Attribute("name:"..lang, iname) end
	end
end

-- Set ele and ele_ft on any object
function SetEleAttributes()
	local ele = Find("ele")
	if ele ~= "" then
		local meter = math.floor(tonumber(ele) or 0)
		local feet = math.floor(meter * 3.2808399)
		AttributeNumeric("ele", meter)
		AttributeNumeric("ele_ft", feet)
	end
end

function SetBrunnelAttributes()
	if Find("bridge") == "yes" or Find("man_made") == "bridge" then Attribute("brunnel", "bridge")
	elseif Find("tunnel") == "yes" then Attribute("brunnel", "tunnel")
	elseif Find("ford")   == "yes" then Attribute("brunnel", "ford")
	end
end

-- Set minimum zoom level by area
function SetMinZoomByArea()
	SetMinZoomByAreaWithLimit(0)
end

-- Set minimum zoom level by area but not below given minzoom
function SetMinZoomByAreaWithLimit(minzoom)
	local area=Area()
	if     minzoom <= 1 and area>ZRES0^2  then MinZoom(1)
	elseif minzoom <= 2 and area>ZRES1^2  then MinZoom(2)
	elseif minzoom <= 3 and area>ZRES2^2  then MinZoom(3)
	elseif minzoom <= 4 and area>ZRES3^2  then MinZoom(4)
	elseif minzoom <= 5 and area>ZRES4^2  then MinZoom(5)
	elseif minzoom <= 6 and area>ZRES5^2  then MinZoom(6)
	elseif minzoom <= 7 and area>ZRES6^2  then MinZoom(7)
	elseif minzoom <= 8 and area>ZRES7^2  then MinZoom(8)
	elseif minzoom <= 9 and area>ZRES8^2  then MinZoom(9)
	elseif minzoom <= 10 and area>ZRES9^2  then MinZoom(10)
	elseif minzoom <= 11 and area>ZRES10^2 then MinZoom(11)
	elseif minzoom <= 12 and area>ZRES11^2 then MinZoom(12)
	elseif minzoom <= 13 and area>ZRES12^2 then MinZoom(13)
	else                      MinZoom(14) end
end

-- Calculate POIs (typically rank 1-4 go to 'poi' z12-14, rank 5+ to 'poi_detail' z14)
-- returns rank, class, subclass
function GetPOIRank()
	local k,list,v,class,rank

	-- Can we find the tag?
	for k,list in pairs(poiTags) do
		if list[Find(k)] then
			v = Find(k)	-- k/v are the OSM tag pair
			class = poiClasses[v] or k
			rank  = poiClassRanks[class] or 25
			subclassKey = poiSubClasses[v]
			if subclassKey then
				class = v
				v = Find(subclassKey)
			end
			return rank, class, v
		end
	end

	-- Catch-all for shops
	local shop = Find("shop")
	if shop~="" then return poiClassRanks['shop'], "shop", shop end

	-- Nothing found
	return nil,nil,nil
end

function SetBuildingHeightAttributes()
	local height = tonumber(Find("height"), 10)
	local minHeight = tonumber(Find("min_height"), 10)
	local levels = tonumber(Find("building:levels"), 10)
	local minLevel = tonumber(Find("building:min_level"), 10)

	local renderHeight = BUILDING_FLOOR_HEIGHT
	if height or levels then
		renderHeight = height or (levels * BUILDING_FLOOR_HEIGHT)
	end
	local renderMinHeight = 0
	if minHeight or minLevel then
		renderMinHeight = minHeight or (minLevel * BUILDING_FLOOR_HEIGHT)
	end

	-- Fix upside-down buildings
	if renderHeight < renderMinHeight then
		renderHeight = renderHeight + renderMinHeight
	end

	AttributeNumeric("render_height", renderHeight)
	AttributeNumeric("render_min_height", renderMinHeight)
end

-- Implement z_order as calculated by Imposm
-- See https://imposm.org/docs/imposm3/latest/mapping.html#wayzorder for details.
function SetZOrder()
	local highway = Find("highway")
	local layer = tonumber(Find("layer"))
	local bridge = Find("bridge")
	local tunnel = Find("tunnel")
	local zOrder = 0
	if bridge ~= "" and bridge ~= "no" then
		zOrder = zOrder + 10
	elseif tunnel ~= "" and tunnel ~= "no" then
		zOrder = zOrder - 10
	end
	if not (layer == nil) then
		if layer > 7 then
			layer = 7
		elseif layer < -7 then
			layer = -7
		end
		zOrder = zOrder + layer * 10
	end
	local hwClass = 0
	-- See https://github.com/omniscale/imposm3/blob/53bb80726ca9456e4a0857b38803f9ccfe8e33fd/mapping/columns.go#L251
	if highway == "motorway" then
		hwClass = 9
	elseif highway == "trunk" then
		hwClass = 8
	elseif highway == "primary" then
		hwClass = 6
	elseif highway == "secondary" then
		hwClass = 5
	elseif highway == "tertiary" then
		hwClass = 4
	else
		hwClass = 3
	end
	zOrder = zOrder + hwClass
	ZOrder(zOrder)
end

-- ==========================================================
-- Lua utility functions

function split(inputstr, sep) -- https://stackoverflow.com/a/7615129/4288232
	if sep == nil then
		sep = "%s"
	end
	local t={} ; i=1
	for str in string.gmatch(inputstr, "([^"..sep.."]+)") do
		t[i] = str
		i = i + 1
	end
	return t
end

-- vim: tabstop=2 shiftwidth=2 noexpandtab
