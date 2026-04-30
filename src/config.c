#include "config.h"
#include "mod_state.h"

static wchar_t g_configPathW[MAX_PATH] = { 0 };

StrayConfig g_config =
{
    VK_F7,
    VK_F8,
    3,
    3,
    0,
    0,
    1,
    "House",
    "Battle",
    "House",
    "TrailerCat",
    "SPECIAL_STRAY_ISAAC",
    "Gun",
    { 9, 9, 9, 9, 9, 9, 9 },
    { 0, 0, 0, 0, 0, 0, 0 },
    { "DefaultMove", "Poke" },
    { "Push", "Dash", "Spin", "FirePunch" },
    { "NeverFull", "FreshMeat" },
    { 2LL, 1LL }
};

static void BuildDefaultConfigPathFromModule(void)
{
    wchar_t modulePathW[MAX_PATH];
    wchar_t* extension;

    g_configPathW[0] = L'\0';
    modulePathW[0] = L'\0';

    if (!g_hModule)
    {
        return;
    }

    if (!GetModuleFileNameW(g_hModule, modulePathW, MAX_PATH))
    {
        return;
    }

    wcsncpy(g_configPathW, modulePathW, MAX_PATH - 1U);
    g_configPathW[MAX_PATH - 1U] = L'\0';
    extension = wcsrchr(g_configPathW, L'.');

    if (extension)
    {
        wcscpy(extension, L".ini");
    }
    else
    {
        wcscat(g_configPathW, L".ini");
    }
}

static int32_t ReadIniInt(const wchar_t* section, const wchar_t* key, int32_t fallbackValue)
{
    wchar_t fallbackText[32];
    wchar_t valueText[64];

    _snwprintf_s(fallbackText, sizeof(fallbackText) / sizeof(fallbackText[0]), _TRUNCATE, L"%d", fallbackValue);
    valueText[0] = L'\0';

    if (g_configPathW[0] == L'\0')
    {
        return fallbackValue;
    }

    // Read as text first to preserve Win32 INI fallback behavior consistently...
    GetPrivateProfileStringW(section, key, fallbackText, valueText, (DWORD)(sizeof(valueText) / sizeof(valueText[0])), g_configPathW);
    return _wtoi(valueText);
}

static void ReadIniText(const wchar_t* section, const wchar_t* key, const char* fallbackValue, char* outValue, size_t outValueSize)
{
    wchar_t fallbackTextW[CONFIG_TEXT_CAPACITY];
    wchar_t valueTextW[CONFIG_TEXT_CAPACITY];
    int converted;

    if (!outValue || outValueSize == 0U)
    {
        return;
    }

    outValue[0] = '\0';
    fallbackTextW[0] = L'\0';
    valueTextW[0] = L'\0';

    // UTF-16 <-> UTF-8, convert both directions...
    if (fallbackValue)
    {
        MultiByteToWideChar(CP_UTF8, 0, fallbackValue, -1, fallbackTextW, (int)(sizeof(fallbackTextW) / sizeof(fallbackTextW[0])));
    }

    if (g_configPathW[0] == L'\0')
    {
        _snprintf_s(outValue, outValueSize, _TRUNCATE, "%s", fallbackValue ? fallbackValue : "");
        return;
    }

    GetPrivateProfileStringW(section, key, fallbackTextW, valueTextW, (DWORD)(sizeof(valueTextW) / sizeof(valueTextW[0])), g_configPathW);
    converted = WideCharToMultiByte(CP_UTF8, 0, valueTextW, -1, outValue, (int)outValueSize, NULL, NULL);

    if (converted <= 0)
    {
        _snprintf_s(outValue, outValueSize, _TRUNCATE, "%s", fallbackValue ? fallbackValue : "");
    }
}

void LoadStrayConfig(void)
{
    // Order matters! (Reads default to whatever is already in g_config)...
    BuildDefaultConfigPathFromModule();

    g_config.spawnTriggerVk = ReadIniInt(L"Spawn", L"HotkeyVk", g_config.spawnTriggerVk);
    g_config.battleDumpVk = ReadIniInt(L"Spawn", L"LogHotkeyVk", g_config.battleDumpVk);
    g_config.spawnSex = ReadIniInt(L"Spawn", L"SpawnSex", g_config.spawnSex);
    ReadIniText(L"Spawn", L"SceneName", g_config.sceneName, g_config.sceneName, sizeof(g_config.sceneName));
    ReadIniText(L"Spawn", L"BattleSceneName", g_config.battleSceneName, g_config.battleSceneName, sizeof(g_config.battleSceneName));
    ReadIniText(L"Spawn", L"HouseComponentType", g_config.houseComponentType, g_config.houseComponentType, sizeof(g_config.houseComponentType));

    ReadIniText(L"Template", L"CustomCatName", g_config.templateName, g_config.templateName, sizeof(g_config.templateName));
    g_config.templateSex = ReadIniInt(L"Template", L"InitSex", g_config.templateSex);
    g_config.templateFlag = ReadIniInt(L"Template", L"InitFlag", g_config.templateFlag);

    ReadIniText(L"SpecialStray", L"LocalizationKey", g_config.localizationKey, g_config.localizationKey, sizeof(g_config.localizationKey));
    ReadIniText(L"SpecialStray", L"InlineNameFallback", g_config.inlineNameFallback, g_config.inlineNameFallback, sizeof(g_config.inlineNameFallback));
    g_config.gender = ReadIniInt(L"SpecialStray", L"Gender", g_config.gender);
    g_config.noBreed = ReadIniInt(L"SpecialStray", L"NoBreed", g_config.noBreed);

    g_config.heritableStats.strength = ReadIniInt(L"Stats", L"Strength", g_config.heritableStats.strength);
    g_config.heritableStats.dexterity = ReadIniInt(L"Stats", L"Dexterity", g_config.heritableStats.dexterity);
    g_config.heritableStats.constitution = ReadIniInt(L"Stats", L"Constitution", g_config.heritableStats.constitution);
    g_config.heritableStats.intelligence = ReadIniInt(L"Stats", L"Intelligence", g_config.heritableStats.intelligence);
    g_config.heritableStats.speed = ReadIniInt(L"Stats", L"Speed", g_config.heritableStats.speed);
    g_config.heritableStats.charisma = ReadIniInt(L"Stats", L"Charisma", g_config.heritableStats.charisma);
    g_config.heritableStats.luck = ReadIniInt(L"Stats", L"Luck", g_config.heritableStats.luck);

    g_config.levellingStats.strength = ReadIniInt(L"StatDeltas", L"Strength", g_config.levellingStats.strength);
    g_config.levellingStats.dexterity = ReadIniInt(L"StatDeltas", L"Dexterity", g_config.levellingStats.dexterity);
    g_config.levellingStats.constitution = ReadIniInt(L"StatDeltas", L"Constitution", g_config.levellingStats.constitution);
    g_config.levellingStats.intelligence = ReadIniInt(L"StatDeltas", L"Intelligence", g_config.levellingStats.intelligence);
    g_config.levellingStats.speed = ReadIniInt(L"StatDeltas", L"Speed", g_config.levellingStats.speed);
    g_config.levellingStats.charisma = ReadIniInt(L"StatDeltas", L"Charisma", g_config.levellingStats.charisma);
    g_config.levellingStats.luck = ReadIniInt(L"StatDeltas", L"Luck", g_config.levellingStats.luck);

    ReadIniText(L"Abilities", L"Basic0", g_config.basicAbilities[0], g_config.basicAbilities[0], sizeof(g_config.basicAbilities[0]));
    ReadIniText(L"Abilities", L"Basic1", g_config.basicAbilities[1], g_config.basicAbilities[1], sizeof(g_config.basicAbilities[1]));
    ReadIniText(L"Abilities", L"Accessible0", g_config.accessibleAbilities[0], g_config.accessibleAbilities[0], sizeof(g_config.accessibleAbilities[0]));
    ReadIniText(L"Abilities", L"Accessible1", g_config.accessibleAbilities[1], g_config.accessibleAbilities[1], sizeof(g_config.accessibleAbilities[1]));
    ReadIniText(L"Abilities", L"Accessible2", g_config.accessibleAbilities[2], g_config.accessibleAbilities[2], sizeof(g_config.accessibleAbilities[2]));
    ReadIniText(L"Abilities", L"Accessible3", g_config.accessibleAbilities[3], g_config.accessibleAbilities[3], sizeof(g_config.accessibleAbilities[3]));

    ReadIniText(L"Passives", L"Passive0", g_config.passiveNames[0], g_config.passiveNames[0], sizeof(g_config.passiveNames[0]));
    ReadIniText(L"Passives", L"Passive1", g_config.passiveNames[1], g_config.passiveNames[1], sizeof(g_config.passiveNames[1]));
    g_config.passiveLevels[0] = (int64_t)ReadIniInt(L"Passives", L"Passive0Level", (int32_t)g_config.passiveLevels[0]);
    g_config.passiveLevels[1] = (int64_t)ReadIniInt(L"Passives", L"Passive1Level", (int32_t)g_config.passiveLevels[1]);

    if (g_mj.Log)
    {
        g_mj.Log(MOD_NAME, "Loaded stray config: template=%s localization=%s spawnHotkey=0x%X logHotkey=0x%X battleScene=%s", g_config.templateName, g_config.localizationKey, g_config.spawnTriggerVk, g_config.battleDumpVk, g_config.battleSceneName);
    }
}
