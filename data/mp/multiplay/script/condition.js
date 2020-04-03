function inOneTeam(playnum, splaynum)
{
//	FIXME allianceExistsBetween dont correct if leave player in ALLIANCES_UNSHARED, ALLIANCES_TEAMS mode
//	and  team is garbage in NO_ALLIANCES, ALLIANCES mode
	if ((alliancesType == ALLIANCES_UNSHARED || alliancesType == ALLIANCES_TEAMS) && (playerData[playnum].team != playerData[splaynum].team))
	{
		return false;
	}
	else if (alliancesType == NO_ALLIANCES && playnum != splaynum)
	{
		return false;
	}
	else if (alliancesType == ALLIANCES && !allianceExistsBetween(playnum, splaynum))
	{
		return false;
	}
	else
	{
		return true;
	}
}

function canPlay(playnum)
{
	var feature = {};
	var structs = [FACTORY, CYBORG_FACTORY, VTOL_FACTORY];

	feature.factory = false;
	for (var splaynum = 0; splaynum < maxPlayers; splaynum++)
	{
		if (!inOneTeam(playnum, splaynum))
		{
			continue;
		}
		for (var i = 0; i < structs.length; ++i)
		{
			var onMapStructss = enumStruct(splaynum, structs[i]);
			for (var j = 0; j < onMapStructss.length; ++j)
			{
				if (onMapStructss[j].status === BUILT)
				{
					feature.factory = true;
					break;
				}
			}
		}
	}

	feature.droid = false;
	for (var splaynum = 0; splaynum < maxPlayers; splaynum++)
	{
		if (inOneTeam(playnum, splaynum) && countDroid(DROID_ANY, splaynum) > 0)	// checking enemy player
		{
			feature.droid = true;
		}
	}

	feature.onlyConstruct = false;
	for (var splaynum = 0; splaynum < maxPlayers; splaynum++)
	{
		if (inOneTeam(playnum, splaynum) && countDroid(DROID_ANY, splaynum) === countDroid(DROID_CONSTRUCT, playnum))
		{
			feature.onlyConstruct = true;
		}
	}

	feature.oilReach=false;
	var oils = enumFeature(ALL_PLAYERS).filter(function(e){if (e.stattype === OIL_RESOURCE) return true; return false;});
	for (var tplaynum = 0; tplaynum < maxPlayers; tplaynum++)
	{
		oils = oils.concat(enumStruct(tplaynum, "A0ResourceExtractor"));
	}
	for (var splaynum = 0; splaynum < maxPlayers; splaynum++)
	{
		if (!inOneTeam(playnum, splaynum))
		{
			continue;
		}
		if (enumStruct(splaynum, RESOURCE_EXTRACTOR).length != 0)
		{
			feature.oilReach = true;
			break;
		}
		else
		{
			var trucks = enumDroid(splaynum, DROID_CONSTRUCT);
			trucks.forEach(function(truck)
			{
				oils.forEach(function (oil)
				{
					if (droidCanReach(truck, oil.x, oil.y))
					{
						feature.oilReach = true;
					}
				});
			});
		}
	}

//	debug(playerData[playnum].name + JSON.stringify(feature));
	if (!feature.factory && !feature.droid)
	{
		return false;
	}
	else if (!feature.factory && !feature.oilReach && feature.onlyConstruct)
	{
		return false;
	}
	return true;
}

function toSpectator(playnum, remove)
{
	setPower(0, playnum);
	addSpotter(1, 1, playnum, 32640, 0, 0);
	var HQstructs = enumStruct(playnum, HQ);
	HQstructs.forEach(function(e){removeObject(e);});
	if (remove === true)
	{
		var droids = enumDroid(playnum, DROID_ANY);
		droids.forEach(function(e){removeObject(e);});
		var structs = enumStruct(playnum);
		structs.forEach(function(e){removeObject(e);});
	}
	if (selectedPlayer === playnum)
	{queue ("hud")}
}

function hud()
{
	setMiniMap(true);
	setReticuleButton(4, "", "", "");
	setReticuleButton(5, "", "image_intelmap_up.png", "image_intelmap_down.png");
	setDesign(false);
	showInterface();
	hackPlayIngameAudio();
}

