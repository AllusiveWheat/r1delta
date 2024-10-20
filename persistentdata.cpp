﻿#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include "bitbuf.h"
#include "cvar.h"
#include "persistentdata.h"
#include "logging.h"
#include "squirrel.h"
#include "keyvalues.h"
#include "factory.h"
// Network message handling
#include <unordered_map>
#include <cstdint>
#include <unordered_set>
#include <shlobj.h>
#include <filesystem>
#include <iostream>
#include <fstream>

#include "load.h"

//#define HASH_USERINFO_KEYS
// Constants
constexpr size_t MAX_LENGTH = 254;
constexpr const char* INVALID_CHARS = "{}()':;`\"\n";
bool g_bNoSendConVar = false;

// Utility functions
bool IsValidUserInfo(const char* value) {
	if (!value) return false;

	return std::strlen(value) <= MAX_LENGTH &&
		std::none_of(value, value + std::strlen(value), [](char c) {
		return std::strchr(INVALID_CHARS, c) != nullptr;
			});
}
std::string hashUserInfoKey(const std::string& key) {
#ifdef HASH_USERINFO_KEYS
	// Hash the key
	std::size_t hash = std::hash<std::string>{}(key);

	// Convert to base36
	const char base36Chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
	std::string result;

	do {
		result.push_back(base36Chars[hash % 36]);
		hash /= 36;
	} while (hash > 0);

	// Reverse the string to get the correct order
	std::reverse(result.begin(), result.end());

	// Truncate to maximum allowed length if necessary
	constexpr size_t MAX_KEY_LENGTH = 254 - sizeof(PERSIST_COMMAND);
	if (result.length() > MAX_KEY_LENGTH) {
		result = result.substr(0, MAX_KEY_LENGTH);
	}

	return result;
#else
	return key;
#endif
}

// Command handling
void setinfopersist_cmd(const CCommand& args) {
	auto engine = G_engine;
	auto setinfo_cmd = decltype(&setinfopersist_cmd)(engine + 0x5B520);
	auto setinfo_cmd_flags = (int*)(engine + 0x05B5FF);
	void(*ccommand_constructor)(CCommand * thisptr, int nArgC, const char** ppArgV) = decltype(ccommand_constructor)(engine + 0x4806F0);

	static bool bUnprotectedFlags = false;
	if (!bUnprotectedFlags) {
		bUnprotectedFlags = true;
		DWORD out;
		VirtualProtect(setinfo_cmd_flags, sizeof(int), PAGE_EXECUTE_READWRITE, &out);
	}

	*setinfo_cmd_flags = FCVAR_PERSIST_MASK;

	if (args.ArgC() >= 3) {
		if (!IsValidUserInfo(args.Arg(1))) {
			Warning("Invalid user info key %s. Only certain characters are allowed.\n", args.Arg(1));
			return;
		}
		if (!IsValidUserInfo(args.Arg(2))) {
			Warning("Invalid user info value %s. Only certain characters are allowed.\n", args.Arg(1));
			return;
		}

		// Check for "nosend" argument, or if the convar does not exist
		bool noSend = (args.ArgC() >= 4 && strcmp(args.Arg(3), "nosend") == 0);
		bool shouldHash = !noSend && (OriginalCCVar_FindVar(cvarinterface, (std::string(PERSIST_COMMAND" ") + hashUserInfoKey(args.Arg(1))).c_str()) == nullptr);
		if (args.ArgC() >= 4 && strcmp(args.Arg(3), "forcehash") == 0)
			noSend = shouldHash = true;
		std::vector<const char*> newArgv(noSend ? args.ArgC() - 1 : args.ArgC());
		newArgv[0] = args.Arg(0);
		char modifiedKey[CCommand::COMMAND_MAX_LENGTH];
		snprintf(modifiedKey, sizeof(modifiedKey), "%s %s", PERSIST_COMMAND, shouldHash ? hashUserInfoKey(args.Arg(1)).c_str() : args.Arg(1));
		newArgv[1] = modifiedKey;

		// Copy arguments, skipping "nosend" if present
		std::copy(args.ArgV() + 2, args.ArgV() + (noSend ? args.ArgC() - 1 : args.ArgC()), newArgv.begin() + 2);

		// Allocate memory for CCommand on the stack
		char commandMemory[sizeof(CCommand)];
		CCommand* pCommand = reinterpret_cast<CCommand*>(commandMemory);

		// Construct the CCommand object using placement new
		ccommand_constructor(pCommand, noSend ? args.ArgC() - 1 : args.ArgC(), newArgv.data());

		// Set the global variable
		g_bNoSendConVar = noSend;
		//if (shouldHash)
		//	Msg("Setting persistent value: key=%s, hashedKey=%s, value=%s, hashed=true\n", args.Arg(1), hashUserInfoKey(args.Arg(1)).c_str(), args.Arg(2));
		//else
		//	Msg("Setting persistent value: key=%s, value=%s, hashed=false\n", args.Arg(1), args.Arg(2));
		// Use the constructed CCommand object
		setinfo_cmd(*pCommand);

		// Reset the global variable
		g_bNoSendConVar = false;

		// Manually call the destructor
		pCommand->~CCommand();
	}
	else if (args.ArgC() == 2) {
		std::string hashedKey = hashUserInfoKey(args.Arg(1));

		char modifiedKey[CCommand::COMMAND_MAX_LENGTH];
		snprintf(modifiedKey, sizeof(modifiedKey), "%s %s", PERSIST_COMMAND, hashedKey.c_str());
		auto hVar = OriginalCCVar_FindVar(cvarinterface, modifiedKey);
		if (hVar)
			ConVar_PrintDescription(hVar);
		else {
			auto result = OriginalCCVar_FindVar(cvarinterface, args.GetCommandString());
			if (result)
				ConVar_PrintDescription(result);
		}
	}
	else {
		setinfo_cmd(args);
	}

	*setinfo_cmd_flags = FCVAR_USERINFO;
}

// ConVar handling
__int64 CConVar__GetSplitScreenPlayerSlot(char* fakethisptr) {
	ConVarR1* thisptr = reinterpret_cast<ConVarR1*>(fakethisptr - 48);
	return (thisptr->m_nFlags & FCVAR_PERSIST) ? -1 : 0;
}

// Global map to store the set of unique convar name hashes per netchannel
std::unordered_map<void*, std::unordered_set<size_t>> g_netChannelUniqueConvarHashes;

bool NET_SetConVar__ReadFromBuffer(NET_SetConVar* thisptr, bf_read& buffer) {
	uint32_t numvars;
	uint8_t byteCount = buffer.ReadByte();

	if (byteCount == static_cast<uint8_t>(-1)) {
		numvars = buffer.ReadUBitVar();
	}
	else {
		numvars = byteCount;
	}

	// Get the current netchannel's unique convar hash set
	auto& uniqueConvarHashes = g_netChannelUniqueConvarHashes[thisptr->m_NetChannel];

	thisptr->m_ConVars.RemoveAll();
	for (uint32_t i = 0; i < numvars; i++) {
		NetMessageCvar_t var;
		buffer.ReadString(var.name, sizeof(var.name));
		buffer.ReadString(var.value, sizeof(var.value));
		// Hash the convar name
		size_t nameHash = std::hash<std::string>{}(var.name);

		// Check if this is a new unique convar name hash
		if (uniqueConvarHashes.find(nameHash) == uniqueConvarHashes.end()) {
			// Check if adding this new unique convar would exceed the limit
			if (uniqueConvarHashes.size() >= 32767) {
				// Limit exceeded, stop processing and return false
				Warning("%s", "Client exceeded 32767 userinfo ConVar key limit");
				return false;
			}
			// Add the new unique convar name hash to the set
			uniqueConvarHashes.insert(nameHash);
		}

		thisptr->m_ConVars.AddToTail(var);
	}

	return !buffer.IsOverflowed();
}

bool NET_SetConVar__WriteToBuffer(NET_SetConVar* thisptr, bf_write& buffer) {
	if (g_bNoSendConVar) {
		// Write 0 ConVars
		buffer.WriteByte(0);
		return !buffer.IsOverflowed();
	}
	if (!IsDedicatedServer()) {
		auto var = OriginalCCVar_FindVar(cvarinterface, "net_secure");
		bool bVanilla = var->m_Value.m_nValue == 1;
		if (bVanilla) {
			for (int i = thisptr->m_ConVars.Count() - 1; i >= 0; --i) {
				if (thisptr->m_ConVars[i].name[0] == '_') {
					thisptr->m_ConVars.Remove(i);
				}
			}
		}
	}

	uint32_t numvars = thisptr->m_ConVars.Count();
	if (numvars < 255) {
		buffer.WriteByte(numvars);
	}
	else {
		buffer.WriteByte(static_cast<uint8_t>(-1));
		buffer.WriteUBitVar(numvars);
	}

	for (uint32_t i = 0; i < numvars; i++) {
		NetMessageCvar_t* var = &thisptr->m_ConVars[i];
		buffer.WriteString(var->name);
		buffer.WriteString(var->value);
	}
	return !buffer.IsOverflowed();
}

// Squirrel VM functions
SQInteger Script_ClientGetPersistentData(HSQUIRRELVM v) {
	if (sq_gettop(nullptr, v) != 3) {
		return sq_throwerror(v, "Expected 2 parameters");
	}

	const SQChar* key;
	if (SQ_FAILED(sq_getstring(v, 2, &key))) {
		return sq_throwerror(v, "Parameter 1 must be a string");
	}
	const SQChar* defaultValue;
	if (SQ_FAILED(sq_getstring(v, 3, &defaultValue))) {
		return sq_throwerror(v, "Parameter 2 must be a string");
	}
	auto hashedKey = hashUserInfoKey(key);
	std::string varName = std::string(PERSIST_COMMAND) + " " + hashedKey;

	if (!IsValidUserInfo(key) || !IsValidUserInfo(varName.c_str()) || !IsValidUserInfo(defaultValue)) {
		return sq_throwerror(v, "Invalid user info key or default value.");
	}

	auto var = OriginalCCVar_FindVar(cvarinterface, varName.c_str());

	if (!var) {
		//Warning("Client couldn't find persistent value: key=%s, hashedKey=%s, hashed=%s\n",
		//    key, hashedKey.c_str(), "true");

		sq_pushstring(v, defaultValue, -1);
	}
	else {
		//Msg("Client accessing persistent value: key=%s, hashedKey=%s, value=%s, hashed=%s\n",
		//    key, hashedKey.c_str(), var->m_Value.m_pszString, "true");
		sq_pushstring(v, var->m_Value.m_pszString, -1);
	}

	return 1;
}
struct CBaseClient
{
	_BYTE gap0[1040];
	KeyValues* m_ConVars;
	char pad[284392];
};
static_assert(sizeof(CBaseClient) == 285440);
struct CBaseClientDS
{
	_BYTE gap0[920];
	KeyValues* m_ConVars;
	char pad[215712];
};
static_assert(sizeof(CBaseClientDS) == 216640);
CBaseClient* g_pClientArray;
CBaseClientDS* g_pClientArrayDS;
KeyValues* GetClientConVarsKV(short index) {
	if (IsDedicatedServer())
		return g_pClientArrayDS[index].m_ConVars;
	else
		return g_pClientArray[index].m_ConVars;
}
SQInteger Script_ServerGetPersistentUserDataKVString(HSQUIRRELVM v) {
	const void* pPlayer = sq_getentity(v, 2);
	if (!pPlayer) {
		return sq_throwerror(v, "player is null");
	}

	const char* pKey, * pDefaultValue;
	sq_getstring(v, 3, &pKey);
	sq_getstring(v, 4, &pDefaultValue);
	std::string hashedKey = hashUserInfoKey(pKey);
	std::string modifiedKey = PERSIST_COMMAND" ";
	modifiedKey += hashedKey;

	if (!IsValidUserInfo(pKey) || !IsValidUserInfo(modifiedKey.c_str()) || !IsValidUserInfo(pDefaultValue)) {
		return sq_throwerror(v, "Invalid user info key or default value.");
	}

	auto edict = *reinterpret_cast<__int64*>(reinterpret_cast<__int64>(pPlayer) + 64);
	auto index = ((edict - reinterpret_cast<__int64>(pGlobalVarsServer->pEdicts)) / 56) - 1;

	if (!GetClientConVarsKV(index) || index == 18) {
		//return sq_throwerror(v, "Client has NULL m_ConVars.");
		//Msg("REPLAY on server tried to access persistent value: key=%s, hashedKey=%s, hashed=%s\n",
		//	pKey, hashedKey.c_str(), "true");

		sq_pushstring(v, pDefaultValue, -1); // I HATE REPLAY
		return 1;
	}

	const char* pResult = GetClientConVarsKV(index)->GetString(modifiedKey.c_str(), pDefaultValue);
	//Msg("Server accessing persistent value: key=%s, hashedKey=%s, value=%s, hashed=%s\n",
	//	pKey, hashedKey.c_str(), pResult, "true");

	sq_pushstring(v, pResult, -1);
	return 1;
}

SQInteger Script_ServerSetPersistentUserDataKVString(HSQUIRRELVM v) {
	static void (*CVEngineServer_ClientCommand)(__int64 a1, __int64 a2, const char* a3, ...) = 0;
	if (!CVEngineServer_ClientCommand && !IsDedicatedServer())
		CVEngineServer_ClientCommand = decltype(CVEngineServer_ClientCommand)(G_engine + 0xFE7F0);
	else if (!CVEngineServer_ClientCommand)
		CVEngineServer_ClientCommand = decltype(CVEngineServer_ClientCommand)(G_engine_ds + 0x6F030);
	const void* pPlayer = sq_getentity(v, 2);
	if (!pPlayer) {
		return sq_throwerror(v, "player is null");
	}

	const char* pKey, * pValue;
	sq_getstring(v, 3, &pKey);
	sq_getstring(v, 4, &pValue);
	std::string hashedKey = hashUserInfoKey(pKey);
	std::string modifiedKey = PERSIST_COMMAND" ";
	modifiedKey += hashedKey;

	if (!IsValidUserInfo(pKey) || !IsValidUserInfo(modifiedKey.c_str()) || !IsValidUserInfo(pValue)) {
		return sq_throwerror(v, "Invalid user info key or value.");
	}
	auto edict = *reinterpret_cast<__int64*>(reinterpret_cast<__int64>(pPlayer) + 64);

	auto index = ((edict - reinterpret_cast<__int64>(pGlobalVarsServer->pEdicts)) / 56) - 1;

	if (!(!GetClientConVarsKV(index) || index == 18)) {
		//return sq_throwerror(v, "Client has NULL m_ConVars.");
		CVEngineServer_ClientCommand(0, edict, PERSIST_COMMAND" \"%s\" \"%s\" nosend", hashedKey.c_str(), pValue);
		GetClientConVarsKV(index)->SetString(modifiedKey.c_str(), pValue);
		//Msg("Server setting persistent value: key=%s, value=%s, hashed=%s\n",
		//	pKey, pValue, "true");
	}
	else {
		//Msg("Trying to set persistent value on REPLAY on server: key=%s, hashedKey=%s, value=%s, hashed=%s\n",
		//	pKey, hashedKey.c_str(), pValue, "true");
	}

	sq_pushstring(v, pValue, -1);
	return 1;
}
typedef char (*CBaseClientState__InternalProcessStringCmdType)(void* thisptr, void* msg, bool bIsHLTV);
CBaseClientState__InternalProcessStringCmdType CBaseClientState__InternalProcessStringCmdOriginal;
char CBaseClientState__InternalProcessStringCmd(void* thisptr, void* msg, bool bIsHLTV) {
	auto engine = G_engine;
	void(*Cbuf_Execute)() = decltype(Cbuf_Execute)(engine + 0x1057C0);
	char ret = CBaseClientState__InternalProcessStringCmdOriginal(thisptr, msg, bIsHLTV);
	Cbuf_Execute(); // fix cbuf overflow on too many stringcmds
	return ret;
}
char __fastcall GetConfigPath(char* outPath, size_t outPathSize, int configType)
{
	CHAR folderPath[MAX_PATH];

	// Get the user's Documents folder path
	if (SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, folderPath) < 0)
	{
		return 0;
	}

	// Determine the subfolder based on configType
	const char* subFolder = (configType == 1) ? "/profile" : "/local";

	// Construct the base path
	char tempPath[512];
	auto size = snprintf(tempPath, sizeof(tempPath), "%s%s%s", folderPath, "/Respawn/Titanfall", subFolder);

	if (size >= 511)
	{
		return 0;
	}

	// Determine the config file name based on configType
	const char* configFile;
	switch (configType)
	{
	case 0:
		configFile = "settings.cfg";
		break;
	case 1:
		configFile = "profile.cfg";
		break;
	case 2:
		configFile = "videoconfig.txt";
		break;
	default:
		configFile = "error.cfg";
		break;
	}

	// Construct the final path
	snprintf(outPath, outPathSize, "%s/%s", tempPath, configFile);

	return 1;
}


char ExecuteConfigFile(int configType) {
	constexpr size_t MAX_PATH_LENGTH = 1024;
	constexpr size_t MAX_BUFFER_SIZE = 1024 * 1024; // 1 MB

	char pathBuffer[MAX_PATH_LENGTH];
	if (!GetConfigPath(pathBuffer, MAX_PATH_LENGTH, configType)) {
		return 0; // Failed to get config path
	}

	std::filesystem::path configPath(pathBuffer);

	if (!std::filesystem::exists(configPath)) {
		return 0; // Config file doesn't exist
	}

	uintmax_t fileSize = std::filesystem::file_size(configPath);
	if (fileSize == 0 || fileSize > MAX_BUFFER_SIZE) {
		return 0; // File is empty or too large
	}

	char* buffer = static_cast<char*>(malloc(fileSize + 1)); // +1 for null terminator
	if (!buffer) {
		return 0; // Memory allocation failed
	}
	auto engine = G_engine;
	void* (*Exec_CmdGuts)(const char* commands, char bUseExecuteCommand) = decltype(Exec_CmdGuts)(engine + 0x01059A0);

	std::ifstream file(configPath, std::ios::binary);
	if (!file.read(buffer, fileSize)) {
		free(buffer);
		return 0; // Failed to read file
	}

	buffer[fileSize] = '\0'; // Null terminate the buffer

	Exec_CmdGuts(buffer, 1);

	free(buffer);
	return 1; // Success
}