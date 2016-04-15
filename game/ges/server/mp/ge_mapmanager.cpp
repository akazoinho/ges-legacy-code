//////////  Copyright � 2008, Goldeneye Source. All rights reserved. ///////////
// 
// File: ge_mapmanager.cpp
// Description: Manages all relevant map data
//
///////////////////////////////////////////////////////////////////////////////

#include "cbase.h"

#include "ge_gamerules.h"
#include "gemp_gamerules.h"
#include "gemp_player.h"
#include "ge_spawner.h"
#include "ge_gameplayresource.h"

#include "ge_loadoutmanager.h"
#include "ge_tokenmanager.h"

#include "ge_utils.h"
#include "script_parser.h"
#include "filesystem.h"

#include "ge_playerspawn.h"
#include "ge_mapmanager.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar ge_mapchooser_avoidteamplay("ge_mapchooser_avoidteamplay", "0", FCVAR_GAMEDLL, "If set to 1, server will avoid choosing maps where the current playercount is above the team threshold.");

CGEMapManager::CGEMapManager(void)
{
	m_pCurrentSelectionData = NULL;
}

CGEMapManager::~CGEMapManager(void)
{
	m_pSelectionData.PurgeAndDeleteElements();
}

// Based off of the parser in loadoutmanager.  Thanks KM!
void CGEMapManager::ParseMapSelectionData(void)
{
	const char *baseDir = "scripts/maps/";
	char filePath[256] = "";

	FileFindHandle_t finder;
	const char *filename = filesystem->FindFirstEx("scripts/maps/*.txt", "MOD", &finder);

	while (filename)
	{
		char mapname[32];
		Q_StrLeft(filename, -4, mapname, 32); // Get rid of the file extension.
		Q_strlower(mapname);

		// First see if we already have this.
		for (int i = 0; i < m_pSelectionData.Count(); i++)
		{
			if (!strcmp(m_pSelectionData[i]->mapname, mapname))
			{
				delete m_pSelectionData[i];
				m_pSelectionData.Remove(i);  // Get rid of it so we can update it.
				break;  // We're just looking for one thing so we don't need to shift everything over and keep scanning like we otherwise would.
			}
		}
		
		MapSelectionData *pMapSelectionData = new MapSelectionData;

		Q_strncpy(pMapSelectionData->mapname, mapname, 32); // We already have our map name.
		
		// Set the defaults for everything else
		
		int *valueptrs[5] = { &pMapSelectionData->baseweight, &pMapSelectionData->minplayers, &pMapSelectionData->maxplayers, &pMapSelectionData->teamthreshold, &pMapSelectionData->resintensity };
		char *keyvalues[5] = { "BaseWeight", "MinPlayers", "MaxPlayers", "TeamThreshold", "ResIntensity" };
		bool hasvalue[5] = { false, false, false, false, false };
		int valuedefaults[5] = { 500, 0, 16, 12, 4 };


		for (int v = 0; v < 5; v++)
		{
			*valueptrs[v] = valuedefaults[v]; // Set our default values incase the file is missing one.
		}
		
		// Load the file
		Q_strncpy(filePath, baseDir, 256);
		Q_strncat(filePath, filename, 256);
		char *contents = (char*)UTIL_LoadFileForMe(filePath, NULL);
		
		if (contents)
		{
			CUtlVector<char*> lines;
			CUtlVector<char*> data;
			Q_SplitString(contents, "\n", lines);

			bool skippingblock = false;

			for (int i = 0; i < lines.Count(); i++)
			{
				// Ignore comments
				if (!Q_strncmp(lines[i], "//", 2))
					continue;

				if (!skippingblock && Q_strstr(lines[i], "{")) // We've found an opening bracket, so we no longer care about anything until we hit the closing bracket.
					skippingblock = true;

				// If we're skipping over a bracketed section just ignore everything until the end.
				if (skippingblock)
				{
					if (!Q_strstr(lines[i], "}")) // We don't actually care about the datablocks right now, so just look for the terminator.
						continue; 
					else
						skippingblock = false;
				}

				// Otherwise try parsing the line: [field]\s+[data]
				if (Q_ExtractData(lines[i], data))
				{	
					for (int v = 0; v < 5; v++)
					{
						if (!Q_strcmp(data[0] , keyvalues[v]))
						{
							if (data.Count() > 1)
								*valueptrs[v] = atoi(data[1]);

							hasvalue[v] = true;

							if (hasvalue[0] && hasvalue[1] && hasvalue[2] && hasvalue[3] && hasvalue[4]) //If we got all of our values why are we still here?
								break;
						}
					}
				}

				// Clear the data set (do not purge!)
				data.RemoveAll();
			}

			m_pSelectionData.AddToTail(pMapSelectionData);

			if (!Q_strcmp("default", mapname)) // If this is our default map data, keep a record of it.
				m_pDefaultSelectionData = pMapSelectionData;

			// NOTE: We do not purge the data!
			ClearStringVector(lines);
		}

		delete[] contents;

		// Get the next one
		filename = filesystem->FindNext(finder);
	}
	
	filesystem->FindClose(finder);
}

void CGEMapManager::ParseCurrentMapData(void)
{
	const char* mapname = gpGlobals->mapname.ToCStr();

	if (!mapname) // We don't have a map yet somehow.
		return;

	m_pCurrentSelectionData = NULL; // Zero this out so we don't keep the pointer to the old map data.
	m_pLoadoutBlacklist.PurgeAndDeleteElements(); // Wipe everything else too.
	m_pMapGamemodes.PurgeAndDeleteElements();
	m_pMapTeamGamemodes.PurgeAndDeleteElements();
	m_pMapGamemodeWeights.RemoveAll();
	m_pMapTeamGamemodeWeights.RemoveAll();

	// We already parsed the selection data so just grab that now.
	for (int i = 0; i < m_pSelectionData.Count(); i++)
	{
		if (!strcmp(m_pSelectionData[i]->mapname, mapname))
		{
			m_pCurrentSelectionData = m_pSelectionData[i];
		}
	}

	if (!m_pCurrentSelectionData) //Make sure we found it.
	{
		Warning("Current map has no script file, attempting to use default!\n");
		mapname = "default";
		m_pCurrentSelectionData = m_pDefaultSelectionData;
	}

	// Find our map file.
	char filename[64];
	Q_snprintf(filename, 64, "scripts/maps/%s.txt", mapname);
	V_FixSlashes(filename);

	Msg("Parsing %s for map data\n", filename);

	char *contents = (char*)UTIL_LoadFileForMe(filename, NULL);

	if (!contents)
	{
		Warning("Failed to find map data scipt file!\n");
		return;
	}

	CUtlVector<char*> lines;
	CUtlVector<char*> data;
	Q_SplitString(contents, "\n", lines);

	enum ReadMode
	{
		RD_NONE = 0,
		RD_BLACKLIST,
		RD_FFAGAMEPLAY,
		RD_TEAMGAMEPLAY,
	};

	int Reading = RD_NONE;

	for (int i = 0; i < lines.Count(); i++)
	{
		// Ignore comments
		if (!Q_strncmp(lines[i], "//", 2))
			continue;

		if (Reading == RD_NONE)
		{
			if (!Q_strncmp(lines[i], "WeaponsetWeights", 12))
				Reading = RD_BLACKLIST;

			if (!Q_strncmp(lines[i], "GamemodeWeights", 15))
				Reading = RD_FFAGAMEPLAY;

			if (!Q_strncmp(lines[i], "TeamGamemodeWeights", 19))
				Reading = RD_TEAMGAMEPLAY;

			continue; // The most that should be after the identifier is the opening bracket.
		}

		if (!Q_strncmp(lines[i], "{", 1)) // Ignore the opening bracket if it's all by itself.  It doesn't actually do anything.
			continue;

		if (!Q_strncmp(lines[i], "}", 1)) // We hit the end of the block, right at the start of the line.
		{
			Reading = RD_NONE;
			continue;
		}

		if (Q_ExtractData(lines[i], data))
		{
			// Parse the line according to the data block we're in.
			if (Reading == RD_BLACKLIST)
			{
				if (data.Count() > 1)
				{
					m_pLoadoutBlacklist.AddToTail(data[0]);
					m_pLoadoutWeightOverrides.AddToTail(atoi(data[1]));
				}
				else
					Warning("Weaponset Override specified without weight, ignoring!\n");
			}
			else if (Reading == RD_FFAGAMEPLAY)
			{
				m_pMapGamemodes.AddToTail(data[0]);

				if (data.Count() > 1)
					m_pMapGamemodeWeights.AddToTail(atoi(data[1]));
				else
					m_pMapGamemodeWeights.AddToTail(500);
			}
			else if (Reading == RD_TEAMGAMEPLAY)
			{
				m_pMapTeamGamemodes.AddToTail(data[0]);

				if (data.Count() > 1)
					m_pMapTeamGamemodeWeights.AddToTail(atoi(data[1]));
				else
					m_pMapTeamGamemodeWeights.AddToTail(500);
			}
		}

		if (Q_strstr(lines[i], "}")) // We hit the end of the block, somewhere on this line.
			Reading = RD_NONE;

		// Clear the data set (do not purge!)
		data.RemoveAll();
	}

	// NOTE: We do not purge the data!
	ClearStringVector(lines);
	delete[] contents;
}

MapSelectionData* CGEMapManager::GetMapSelectionData(const char *mapname)
{
	for (int i = 0; i < m_pSelectionData.Count(); i++)
	{
		if (!strcmp(m_pSelectionData[i]->mapname, mapname))
			return m_pSelectionData[i];
	}

	return NULL; // We didn't find it, so they get nothing!
}

void CGEMapManager::GetMapGameplayList(CUtlVector<char*> &gameplays, CUtlVector<int> &weights, bool teamplay)
{
	gameplays.RemoveAll();
	weights.RemoveAll();

	if (teamplay)
	{
		if (m_pMapTeamGamemodes.Count())
		{
			for (int i = 0; i < m_pMapTeamGamemodes.Count(); i++)
			{
				if (GEGameplay()->IsValidGamePlay(m_pMapTeamGamemodes[i])) // Can't do this check when parsing because everything hasn't initialized yet.
				{
					gameplays.AddToTail(m_pMapTeamGamemodes[i]);
					weights.AddToTail(m_pMapTeamGamemodeWeights[i]);
				}
				else
					Warning("Tried to proccess invalid team gamemode from script file, %s\n", m_pMapTeamGamemodes[i]);
			}
		}
		else
			Warning("Map has no team gamemodes listed in script file!\n");
	}
	else
	{
		if (m_pMapGamemodes.Count())
		{
			for (int i = 0; i < m_pMapGamemodes.Count(); i++)
			{
				if (GEGameplay()->IsValidGamePlay(m_pMapGamemodes[i])) // Can't do this check when parsing because everything hasn't initialized yet.
				{
					gameplays.AddToTail(m_pMapGamemodes[i]);
					weights.AddToTail(m_pMapGamemodeWeights[i]);
				}
				else
					Warning("Tried to proccess invalid gamemode from script file, %s\n", m_pMapGamemodes[i]);
			}
		}
		else
			Warning("Map has no gamemodes listed in script file!\n");
	}
}

void CGEMapManager::GetSetBlacklist(CUtlVector<char*> &sets, CUtlVector<int> &weights)
{
	sets.RemoveAll();
	weights.RemoveAll();

	if (m_pLoadoutBlacklist.Count())
	{
		sets.AddVectorToTail(m_pLoadoutBlacklist); // Don't need to check validity because a listing is only relevant when it actually matches a real set.
		weights.AddVectorToTail(m_pLoadoutWeightOverrides);
	}
}

const char* CGEMapManager::SelectNewMap( void )
{
	if (!m_pSelectionData.Count()) // We don't actually have any maps to choose from.
		return NULL;

	int iNumPlayers = 0; // Get all the players currently in the game, spectators might decide they want to play the next map so we should count them too.

	FOR_EACH_MPPLAYER(pPlayer)
		if (pPlayer->IsConnected())
			iNumPlayers++;
	END_OF_PLAYER_LOOP()

	int currentResIntensity = 3;
	CUtlVector<char*> mapnames;
	CUtlVector<int> mapweights;
	CUtlVector<int> mapintensities;
	bool lookforcheap = false;
	int	currentmapindex = -1;

	if (m_pCurrentSelectionData)
		currentResIntensity = m_pCurrentSelectionData->resintensity;

	Msg("---Choosing Map for playercount %d---\n", iNumPlayers);

	for (int i = 0; i < m_pSelectionData.Count(); i++)
	{
		int lowerbound = m_pSelectionData[i]->minplayers - 1;
		int upperbound = m_pSelectionData[i]->maxplayers + 1;

		if (ge_mapchooser_avoidteamplay.GetBool()) // If we want to avoid teamplay our playercount has to come in below the teamthresh.
			upperbound = min(m_pSelectionData[i]->teamthreshold, m_pSelectionData[i]->maxplayers + 1);

		Msg("Looking at %s, with high %d, low %d\n", m_pSelectionData[i]->mapname, upperbound, lowerbound);
		if (lowerbound < iNumPlayers && upperbound > iNumPlayers && engine->IsMapValid(m_pSelectionData[i]->mapname)) //It's within range, calculate weight adjustment.
		{
			mapnames.AddToTail(m_pSelectionData[i]->mapname);

			int mapweight = m_pSelectionData[i]->baseweight;
			float playerradius = (upperbound - lowerbound) * 0.5;
			float centercount = (upperbound + lowerbound) * 0.5;
			float weightscale = 1 - abs(centercount - (float)iNumPlayers) / playerradius;
			int finalweight = round(mapweight * weightscale);

			if (m_pSelectionData[i] == m_pCurrentSelectionData)
				currentmapindex = mapnames.Count() - 1; // Record the current map index so we can remove it later if we have any other viable maps.

			Msg("Added %s with weight %d\n", m_pSelectionData[i]->mapname, finalweight);

			mapweights.AddToTail(finalweight);
			mapintensities.AddToTail(m_pSelectionData[i]->resintensity);

			if (m_pSelectionData[i]->resintensity + currentResIntensity < 10 && m_pSelectionData[i] != m_pCurrentSelectionData) // We have at least one map that comes in under the resource limit.
				lookforcheap = true;  // If we have at least one we can strip out the others and go straight to it.
		}
	}

	if (currentmapindex != -1 && mapnames.Count() > 1) // Our current map is in the list and we have another map to use.
	{
		Msg("Removing current map, which is %s\n", mapnames[currentmapindex]);
		mapnames.Remove(currentmapindex);
		mapweights.Remove(currentmapindex);
		mapintensities.Remove(currentmapindex);
	}

	// Strip out maps that risk a game crash if we can do that.
	if (lookforcheap)
	{
		for (int i = 0; i < mapnames.Count(); )
		{
			if (mapintensities[i] + currentResIntensity >= 10)
			{
				Msg("Removing %s, because it has total resintensity %d\n", mapnames[i], mapintensities[i] + currentResIntensity);
				mapnames.Remove(i);
				mapweights.Remove(i);
				mapintensities.Remove(i);
			}
			else
				i++;
		}
	}

	Msg("---Map list finished with %d maps total---\n", mapnames.Count());

	return GERandomWeighted<char*>(mapnames.Base(), mapweights.Base(), mapnames.Count());
}

void CGEMapManager::PrintMapSelectionData(void)
{
	for (int i = 0; i < m_pSelectionData.Count(); i++)
	{
		Msg("Mapname: %s\n", m_pSelectionData[i]->mapname);
		Msg("\tWeight: %d\n", m_pSelectionData[i]->baseweight);
		Msg("\tResIntensity: %d\n", m_pSelectionData[i]->resintensity);
		Msg("\tMinPlayers: %d\n", m_pSelectionData[i]->minplayers);
		Msg("\tMaxPlayers: %d\n", m_pSelectionData[i]->maxplayers);
		Msg("\tTeamThresh: %d\n", m_pSelectionData[i]->teamthreshold);
	}
}

void CGEMapManager::PrintMapDataLists(void)
{
	Msg("Mapname: %s\n", m_pCurrentSelectionData->mapname);
	Msg("Weight: %d\n", m_pCurrentSelectionData->baseweight);
	Msg("ResIntensity: %d\n", m_pCurrentSelectionData->resintensity);
	Msg("MinPlayers: %d\n", m_pCurrentSelectionData->minplayers);
	Msg("MaxPlayers: %d\n", m_pCurrentSelectionData->maxplayers);
	Msg("TeamThresh: %d\n", m_pCurrentSelectionData->teamthreshold);

	for (int i = 0; i < m_pLoadoutBlacklist.Count(); i++)
	{
		Msg("\t%s\n", m_pLoadoutBlacklist[i]);
	}

	Msg("Gamemode Weights\n");

	for (int i = 0; i < m_pMapGamemodes.Count(); i++)
	{
		Msg("\t%s\t%d\n", m_pMapGamemodes[i], m_pMapGamemodeWeights[i]);
	}

	Msg("Team Gamemode Weights\n");

	for (int i = 0; i < m_pMapTeamGamemodes.Count(); i++)
	{
		Msg("\t%s\t%d\n", m_pMapTeamGamemodes[i], m_pMapTeamGamemodeWeights[i]);
	}
}

CON_COMMAND(ge_print_map_selection_data, "Prints the server's map selection data ")
{
	if (!UTIL_IsCommandIssuedByServerAdmin())
	{
		Msg("You must be a server admin to use this command\n");
		return;
	}

	if (GEMPRules())
		GEMPRules()->GetMapManager()->PrintMapSelectionData();
}

CON_COMMAND(ge_print_current_map_data, "Prints the current map's data ")
{
	if (!UTIL_IsCommandIssuedByServerAdmin())
	{
		Msg("You must be a server admin to use this command\n");
		return;
	}

	if (GEMPRules())
		GEMPRules()->GetMapManager()->PrintMapDataLists();
}