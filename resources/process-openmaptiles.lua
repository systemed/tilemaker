-- Data processing based on openmaptiles.org schema
-- https://openmaptiles.org/schema/
-- Copyright (c) 2016, KlokanTech.com & OpenMapTiles contributors.
-- Used under CC-BY 4.0

-- Nodes will only be processed if one of these keys is present

node_keys = { "amenity", "shop", "sport", "tourism", "place", "office", "natural", "addr:housenumber" }

-- Initialize Lua logic

function init_function()
end

-- Finalize Lua logic()
function exit_function()
end

-- Assign nodes to a layer, and set attributes, based on OSM tags

function node_function(node)
	local amenity = node:Find("amenity")
	local shop = node:Find("shop")
	local sport = node:Find("sport")
	local tourism = node:Find("tourism")
	local office = node:Find("office")
	local housenumber = node:Find("addr:housenumber")

	if amenity~="" or shop~="" or sport~="" or tourism ~= "" or office ~= "" then
		
		local rank = 10
		local class = ""
		if amenity~="" then 
			rank = 4
			class = amenity
		elseif shop~="" then 
			rank = 5 
			class = shop
		elseif sport~="" then 
			rank = 6
			class = sport
		elseif tourism~="" then 
			rank = 3
			class = tourism
		elseif office~="" then 
			rank = 7
			class = office
		end
		if rank >= 4 then
			node:Layer("poi", false)
		else
			node:Layer("poi_detail", false)
		end

		node:Attribute("class", class)
		node:AttributeNumeric("rank", rank)
	end

	local place = node:Find("place")
	if place ~= "" then
		local rank = 1

		if place == "continent" then rank = 1 end
		if place == "country" then rank = 1 end
		if place == "state" then rank = 1 end
		if place == "city" then rank = 2 end
		if place == "town" then rank = 3 end
		if place == "village" then rank = 4 end
		if place == "suburb" then rank = 3 end
		if place == "neighbourhood" then rank = 3 end
		if place == "locality" then rank = 4 end		
		if place == "hamlet" then rank = 4 end

		if rank <= 3 then
			node:Layer("place", false)
		else
			node:Layer("place_detail", false)
		end
		node:AttributeNumeric("rank", rank)
		node:Attribute("class", place)
	end

	if natural ~= "" then
		if natural == "peak" then
			node:Layer("mountain_peak", false)
			local ele = node:Find("ele")
			if ele ~= "" then
				node:AttributeNumeric("ele", ele)
			end
			node:AttributeNumeric("rank", 5)
		end
	end

	if housenumber~="" then
		node:Layer("housenumber", false)
		node:Attribute("housenumber", housenumber)
		return
	end

	local name = node:Find("name")
	node:Attribute("name", name)
	local name_en = name
	local name_de = name
	if node:Find("name:en") ~= "" then
		name_en = node:Find("name:en")
	end
	if node:Find("name:de") ~= "" then
		name_de = node:Find("name:de")
	end
	node:Attribute("name_en", name_en)
	node:Attribute("name_de", name_de)

end

function Set (list)
	local set = {}
	for _, l in ipairs(list) do set[l] = true end
	return set
end

-- https://stackoverflow.com/a/7615129/4288232
function split(inputstr, sep)
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

-- Similarly for ways

function way_function(way)
	local highway = way:Find("highway")
	local waterway = way:Find("waterway")
	local building = way:Find("building")
	local natural = way:Find("natural")
	local landuse = way:Find("landuse")
	local leisure = way:Find("leisure")
	local amenity = way:Find("amenity")
	local aeroway = way:Find("aeroway")
	local railway = way:Find("railway")
	local man_made = way:Find("man_made")
	local disused = way:Find("disused")
	local isClosed = way:IsClosed()

	if disused == "yes" or disused == "1" or disused == "true" then
		return
	end

	if highway~="" then
		way:Layer("transportation", false)
		local minorRoadValues = Set { "unclassified", "residential", "road" }
		if minorRoadValues[highway] then highway = "minor" end

		local trackValues = Set { "cycleway", "byway", "bridleway" }
		if trackValues[highway] then highway = "track" end

		local pathValues = Set { "footway" }
		if pathValues[highway] then highway = "path" end

		local service = way:Find("service")
		if highway == "service" and service ~="" then
			way:Attribute("service", service)
		end

		local linkValues = Set { "motorway_link", "trunk_link", "primary_link", "secondary_link", "tertiary_link" }
		if linkValues[highway] then 
			splitHighway = split(highway, "_")
			highway = splitHighway[1]
			way:AttributeNumeric("ramp",1)
		end

		way:Attribute("class", highway)

		local bridge = way:Find("bridge")
		local tunnel = way:Find("tunnel")
		local ford = way:Find("ford")
		if bridge == "yes" then
			way:Attribute("brunnel", "bridge")
		elseif tunnel == "yes" then
			way:Attribute("brunnel", "tunnel")
		elseif ford == "yes" then
			way:Attribute("brunnel", "ford")
		end

		local oneway = way:Find("oneway")
		if oneway == "yes" or oneway == "1" then
			way:AttributeNumeric("oneway",1)
		end
		if oneway == "-1" then
			-- TODO
		end

--		way:Attribute("id",way:Id())
--		way:AttributeNumeric("area",37)
	end
	if railway~="" then
		way:Layer("transportation", false)
		way:Attribute("class", railway)
	end
	if waterway~="" then
		if waterway == "riverbank" then
			way:Layer("water", isClosed)
			way:Attribute("class", "river")
		elseif waterway == "dock" then
			way:Layer("water", isClosed)
			way:Attribute("class", "lake")	
		elseif waterway == "boatyard" then
			way:Layer("landuse", isClosed)
			way:Attribute("class", "industrial")
		elseif waterway == "dam" then
			way:Layer("building", isClosed)
		elseif waterway == "fuel" then
			way:Layer("landuse", isClosed)
			way:Attribute("class", "industrial")
		else
			way:Layer("waterway", false)
			way:Attribute("class", waterway)
		end
	end
	if building~="" then
		way:Layer("building", true)
	end
	if natural~="" then
		local landcoverkeys = Set { "wood" }

		if natural=="water" then
			local covered = way:Find("covered")
			local river = way:Find("river")
			if covered ~= "yes" then
				way:Layer("water", true)
				if river == "yes" then
					way:Attribute("class", "river")
				else
					way:Attribute("class", "lake")
				end
			end
		end
		if landcoverkeys[natural] then
			way:Layer("landcover", true)
			way:Attribute("class", natural)
		end
		if natural=="glacier" then
			way:Layer("landcover", true)
			way:Attribute("class", "ice")
		end
		if natural=="grassland" then
			way:Layer("landcover", true)
			way:Attribute("class", "grass")
			way:Attribute("subclass", natural)
		end
	end
	if landuse~="" then
		local subclass = ""
		local landcover = ""
		local water = ""

		local farmValues = Set { "field", "farmyard", "allotments", "orchard", "vineyard", "plant_nursery", "farmland"}

		if farmValues[landuse] then 
			landcover = "farmland" 
			if landuse == "field" then landuse = "farmland" end
			if landuse == "farmyard" then landuse = "farm" end
			subclass = landuse
		end
		if landuse == "meadow" then 
			local meadow = way:Find("meadow")
			if meadow == "agricultural" then
				landcover = "farmland" 
				subclass = landuse
			else
				landcover = "grass"
			end
		end
		if landuse == "recreation_ground" then 
			landcover = "grass" 
			subclass = "recreation_ground"
		end
		if landuse == "village_green" then 
			landcover = "grass" 
			subclass = "village_green"
		end
		if landuse == "wetland" then 
			landcover = "wetland" 
			subclass = way:Find("wetland")
		end
		if landuse == "grass" then 
			landcover = "grass" 
		end
		if landuse == "reservoir" then 
			water = "lake" 
		end
		if landuse=="park" then --Incorrect tag but process it anyway
			way:Layer("landcover", true)
			way:Attribute("class", "grass")
			way:Attribute("subclass", "park")
		end

		if landcover ~= "" then
			way:Layer("landcover", true)
			way:Attribute("class", landcover)
		elseif water ~= "" then
			way:Layer("water", true)
			way:Attribute("class", water)
		else
			way:Layer("landuse", true)
			way:Attribute("class", landuse)
		end
		if subclass ~= "" then way:Attribute("subclass", subclass) end
	end
	if amenity~="" then
		local landusekeys = Set { "school", "university", "kindergarten", "college", "library", "hospital"}
		if landusekeys[amenity] then
			way:Layer("landuse", true)
			way:Attribute("class", amenity)
		end
	end
	if leisure~="" then
		if leisure=="nature_reserve" then
			way:Layer("park", true)
			way:Attribute("class", leisure)			
		end
		if leisure=="stadium" then
			way:Layer("landuse", true)
			way:Attribute("class", leisure)
		end
		if leisure=="park" then
			way:Layer("landcover", true)
			way:Attribute("class", "grass")
			way:Attribute("subclass", "park")
		end
		if leisure=="common" then
			way:Layer("landcover", true)
			way:Attribute("class", "grass")
			way:Attribute("subclass", "park")
		end
		if leisure=="golf_course" then
			way:Layer("landcover", true)
			way:Attribute("class", "grass")
		end
		if leisure=="pitch" then
			way:Layer("landcover", true)
			way:Attribute("class", "grass")
		end
		if leisure == "recreation_ground" then -- Wrong tag but process anyway
			way:Layer("landcover", true)
			way:Attribute("class", "grass")
			way:Attribute("subclass", "recreation_ground")
		end

	end
	if aeroway~="" then
		way:Layer("aeroway", isClosed)
	end
	if man_made~="" then

	end

end

