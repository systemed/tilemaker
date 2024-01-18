-- Enter/exit Tilemaker
function init_function()
end
function exit_function()
end

node_keys = {}
function node_function()
end

function way_function()
end

-- Remap coastlines and landcover
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
