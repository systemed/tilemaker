-- Data processing based on openmaptiles.org schema
-- https://openmaptiles.org/schema/
-- Copyright (c) 2016, KlokanTech.com & OpenMapTiles contributors.
-- Used under CC-BY 4.0

--------
-- Alter these lines to control which languages are written for place/streetnames
-- Preferred language can be (for example) "en" for English, "de" for German, or nil to use OSM's name tag:
preferred_language = "cy"
-- This is written into the following vector tile attribute:
preferred_language_attribute = "name:latin"
-- If OSM's name tag differs, then write it into this attribute:
default_language_attribute = "name_int"
-- Also write these languages if they differ - for example, { "de", "fr" }
additional_languages = { "en","de","fr" }
--------

-- Enter/exit Tilemaker
function init_function()
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
ZRES5  = 4891.97
ZRES6  = 2445.98
ZRES7  = 1222.99
ZRES8  = 611.5
ZRES9  = 305.7
ZRES10 = 152.9
ZRES11 = 76.4
ZRES12 = 38.2
ZRES13 = 19.1

-- Process node/way tags
aerodromeValues = Set { "international", "public", "regional", "military", "private" }

-- Process node tags

node_keys = { "amenity", "shop", "sport", "tourism", "place", "office", "natural", "addr:housenumber", "aeroway" }
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
		local rank = nil
		local mz = 13
		local pop = tonumber(Find("population")) or 0

		if     place == "continent"     then mz=2
		elseif place == "country"       then mz=3; rank=1
		elseif place == "state"         then mz=4; rank=2
		elseif place == "city"          then mz=5; rank=3
		elseif place == "town" and pop>8000 then mz=7
		elseif place == "town"          then mz=8
		elseif place == "village" and pop>2000 then mz=9
		elseif place == "village"       then mz=10
		elseif place == "suburb"        then mz=11
		elseif place == "hamlet"        then mz=12
		elseif place == "neighbourhood" then mz=13
		elseif place == "locality"      then mz=13
		end

		Layer("place", false)
		Attribute("class", place)
		MinZoom(mz)
		if rank then AttributeNumeric("rank", rank) end
		SetNameAttributes()
		return
	end

	-- Write 'poi'
	local rank, class, subclass = GetPOIRank()
	if rank then WritePOI(node,class,subclass,rank) end

	-- Write 'mountain_peak' and 'water_name'
	local natural = Find("natural")
	if natural == "peak" or natural == "volcano" then
		Layer("mountain_peak", false)
		SetEleAttributes()
		AttributeNumeric("rank", 1)
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

majorRoadValues = Set { "motorway", "trunk", "primary" }
mainRoadValues  = Set { "secondary", "motorway_link", "trunk_link", "primary_link", "secondary_link" }
midRoadValues   = Set { "tertiary", "tertiary_link" }
minorRoadValues = Set { "unclassified", "residential", "road", "living_street" }
trackValues     = Set { "cycleway", "byway", "bridleway", "track" }
pathValues      = Set { "footway", "path", "steps" }
linkValues      = Set { "motorway_link", "trunk_link", "primary_link", "secondary_link", "tertiary_link" }
constructionValues = Set { "primary", "secondary", "tertiary", "motorway", "service", "trunk", "track" }

aerowayBuildings= Set { "terminal", "gate", "tower" }
landuseKeys     = Set { "school", "university", "kindergarten", "college", "library", "hospital",
                        "railway", "cemetery", "military", "residential", "commercial", "industrial",
                        "retail", "stadium", "pitch", "playground", "theme_park", "bus_station", "zoo" }
landcoverKeys   = { wood="wood", forest="wood",
                    wetland="wetland",
                    beach="sand", sand="sand",
                    farmland="farmland", farm="farmland", orchard="farmland", vineyard="farmland", plant_nursery="farmland",
                    glacier="ice", ice_shelf="ice",
                    grassland="grass", grass="grass", meadow="grass", allotments="grass", park="grass", village_green="grass", recreation_ground="grass", garden="grass", golf_course="grass" }

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
poiClassRanks   = { hospital=1, railway=2, bus=3, attraction=4, harbor=5, college=6,
					school=7, stadium=8, zoo=9, town_hall=10, campsite=11, cemetery=12,
					park=13, library=14, police=15, post=16, golf=17, shop=18, grocery=19,
					fast_food=20, clothing_store=21, bar=22 }
poiKeys         = Set { "amenity", "sport", "tourism", "office", "historic", "leisure", "landuse", "information" }
waterClasses    = Set { "river", "riverbank", "stream", "canal", "drain", "ditch", "dock" }
waterwayClasses = Set { "stream", "river", "canal", "drain", "ditch" }


function way_function()
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
	local sport    = Find("sport")
	local shop     = Find("shop")
	local tourism  = Find("tourism")
	local man_made = Find("man_made")
	local isClosed = IsClosed()
	local housenumber = Find("addr:housenumber")
	local write_name = false
	local construction = Find("construction")

	-- Miscellaneous preprocessing
	if Find("disused") == "yes" then return end
	if highway == "proposed" then return end
	if aerowayBuildings[aeroway] then building="yes"; aeroway="" end
	if landuse == "field" then landuse = "farmland" end
	if landuse == "meadow" and Find("meadow")=="agricultural" then landuse="farmland" end

	-- Roads ('transportation' and 'transportation_name', plus 'transportation_name_detail')
	if highway~="" then
		local h = highway
		local layer = "transportation_detail"
		if majorRoadValues[highway] then              layer="transportation" end
		if mainRoadValues[highway]  then              layer="transportation_main" end
		if midRoadValues[highway]   then              layer="transportation_mid" end
		if minorRoadValues[highway] then h = "minor"; layer="transportation_mid" end
		if trackValues[highway]     then h = "track"; layer="transportation_detail" end
		if pathValues[highway]      then h = "path" ; layer="transportation_detail" end
		if h=="service"             then              layer="transportation_detail" end
		Layer(layer, false)
		Attribute("class", h)
		SetBrunnelAttributes()

		-- Construction
		if highway == "construction" then
			if constructionValues[construction] then
				Attribute("class", construction .. "_construction")
			else
				Attribute("class", "minor_construction")
			end
		end

		-- Service
		local service = Find("service")
		if highway == "service" and service ~="" then Attribute("service", service) end

		-- Links (ramp)
		if linkValues[highway] then
			splitHighway = split(highway, "_")
			highway = splitHighway[1]
			AttributeNumeric("ramp",1)
		end

		local oneway = Find("oneway")
		if oneway == "yes" or oneway == "1" then
			AttributeNumeric("oneway",1)
		end
		if oneway == "-1" then
			-- **** TODO
		end

		-- Write names
		if layer == "motorway" or layer == "trunk" then
			Layer("transportation_name", false)
		elseif h == "minor" or h == "track" or h == "path" or h == "service" then
			Layer("transportation_name_detail", false)
		else
			Layer("transportation_name_mid", false)
		end
		SetNameAttributes()
		Attribute("class",h)
		Attribute("network","road") -- **** needs fixing
		if h~=highway then Attribute("subclass",highway) end
		local ref = Find("ref")
		if ref~="" then
			Attribute("ref",ref)
			AttributeNumeric("ref_length",ref:len())
		end
	end

	-- Railways ('transportation' and 'transportation_name', plus 'transportation_name_detail')
	if railway~="" then
		Layer("transportation", false)
		Attribute("class", railway)

		Layer("transportation_name", false)
		SetNameAttributes()
		MinZoom(14)
		Attribute("class", "rail")
	end

	-- 'Aeroway'
	if aeroway~="" then
		Layer("aeroway", isClosed)
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
	if waterwayClasses[waterway] and not isClosed then
		if waterway == "river" and Holds("name") then
			Layer("waterway", false)
		else
			Layer("waterway_detail", false)
		end
		if Find("intermittent")=="yes" then AttributeNumeric("intermittent", 1) else AttributeNumeric("intermittent", 0) end
		Attribute("class", waterway)
		SetNameAttributes()
		SetBrunnelAttributes()
	elseif waterway == "boatyard"  then Layer("landuse", isClosed); Attribute("class", "industrial")
	elseif waterway == "dam"       then Layer("building",isClosed)
	elseif waterway == "fuel"      then Layer("landuse", isClosed); Attribute("class", "industrial")
	end
	-- Set names on rivers
	if waterwayClasses[waterway] and not isClosed then
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
		SetMinZoomByArea()
	end

	-- Set 'housenumber'
	if housenumber~="" then
		LayerAsCentroid("housenumber", false)
		Attribute("housenumber", housenumber)
	end

	-- Set 'water'
	if natural=="water" or natural=="bay" or leisure=="swimming_pool" or landuse=="reservoir" or landuse=="basin" or waterClasses[waterway] then
		if Find("covered")=="yes" or not isClosed then return end
		local class="lake"; if natural=="bay" then class="ocean" elseif waterway~="" then class="river" end
		Layer("water",true)
--		SetMinZoomByArea()
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
--			SetMinZoomByArea()
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
			write_name = true
		end
	end

	-- Parks
	if     boundary=="national_park" then Layer("park",true); Attribute("class",boundary); SetNameAttributes()
	elseif leisure=="nature_reserve" then Layer("park",true); Attribute("class",leisure ); SetNameAttributes() end

	-- POIs ('poi' and 'poi_detail')
	local rank, class, subclass = GetPOIRank()
	if rank then WritePOI(way,class,subclass,rank); return end

	-- Catch-all
	if (building~="" or write_name) and Holds("name") then
		LayerAsCentroid("poi_detail")
		SetNameAttributes()
		if write_name then rank=6 else rank=25 end
		AttributeNumeric("rank", rank)
	end
end

-- Remap coastlines
function attribute_function(attr)
	return { class="ocean" }
end

-- ==========================================================
-- Common functions

-- Write a way centroid to POI layer
function WritePOI(obj,class,subclass,rank)
	local layer = "poi"
	if rank>4 then layer="poi_detail" end
	LayerAsCentroid(layer)
	SetNameAttributes(obj)
	AttributeNumeric("rank", rank)
	Attribute("class", class)
	Attribute("subclass", subclass)
end

-- Set name attributes on any object
function SetNameAttributes(obj)
	local name = Find("name")
	local main_written = name
	local iname
	-- if we have a preferred language, then write that (if available), and additionally write the base name tag
	if preferred_language and Holds("name:"..preferred_language) then 
		iname = Find("name:"..preferred_language)
print("Found "..preferred_language..": "..iname)
		Attribute(preferred_language_attribute, iname)
		if iname~=name and default_language_attribute then
			Attribute(default_language_attribute, name)
		else main_written = iname end
	else
		Attribute(preferred_language_attribute, name)
	end
	-- then set any additional languages
	for i,lang in ipairs(additional_languages) do
		iname = Find("name:"..lang)
		if iname=="" then iname=name end
		if iname~=main_written then Attribute("name:"..lang, iname) end
	end
end

-- Set ele and ele_ft on any object
function SetEleAttributes(obj)
    local ele = Find("ele")
	if ele ~= "" then
		local meter = math.floor(tonumber(ele) or 0)
		local feet = math.floor(meter * 3.2808399)
		AttributeNumeric("ele", meter)
		AttributeNumeric("ele_ft", feet)
    end
end

function SetBrunnelAttributes(obj)
	if     Find("bridge") == "yes" then Attribute("brunnel", "bridge")
	elseif Find("tunnel") == "yes" then Attribute("brunnel", "tunnel")
	elseif Find("ford")   == "yes" then Attribute("brunnel", "ford")
	end
end

-- Set minimum zoom level by area
function SetMinZoomByArea()
	local area=Area()
	if     area>ZRES5^2  then MinZoom(6)
	elseif area>ZRES6^2  then MinZoom(7)
	elseif area>ZRES7^2  then MinZoom(8)
	elseif area>ZRES8^2  then MinZoom(9)
	elseif area>ZRES9^2  then MinZoom(10)
	elseif area>ZRES10^2 then MinZoom(11)
	elseif area>ZRES11^2 then MinZoom(12)
	elseif area>ZRES12^2 then MinZoom(13)
	else                      MinZoom(14) end
end

-- Calculate POIs (typically rank 1-4 go to 'poi' z12-14, rank 5+ to 'poi_detail' z14)
-- returns rank, class, subclass
function GetPOIRank(obj)
	local k,list,v,class,rank

	-- Can we find the tag?
	for k,list in pairs(poiTags) do
		if list[Find(k)] then
			v = Find(k)	-- k/v are the OSM tag pair
			class = poiClasses[v] or v
			rank  = poiClassRanks[class] or 25
			return rank, class, v
		end
	end

	-- Catch-all for shops
	local shop = Find("shop")
	if shop~="" then return poiClassRanks['shop'], "shop", shop end

	-- Nothing found
	return nil,nil,nil
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

