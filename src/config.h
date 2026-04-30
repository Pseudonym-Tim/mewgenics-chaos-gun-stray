#pragma once

#include "game_runtime_types.h"

typedef struct StrayConfig
{
    int32_t spawnTriggerVk;
    int32_t battleDumpVk;
    int32_t spawnSex;
    int32_t templateSex;
    int32_t templateFlag;
    int32_t gender;
    int32_t noBreed;
    char sceneName[CONFIG_TEXT_CAPACITY];
    char battleSceneName[CONFIG_TEXT_CAPACITY];
    char houseComponentType[CONFIG_TEXT_CAPACITY];
    char templateName[CONFIG_TEXT_CAPACITY];
    char localizationKey[CONFIG_TEXT_CAPACITY];
    char inlineNameFallback[CONFIG_TEXT_CAPACITY];
    CatStats heritableStats;
    CatStats levellingStats;
    char basicAbilities[CATDATA_BASIC_ACTIVE_COUNT][CONFIG_TEXT_CAPACITY];
    char accessibleAbilities[CATDATA_ACCESSIBLE_ACTIVE_COUNT][CONFIG_TEXT_CAPACITY];
    char passiveNames[CATDATA_PASSIVE_COUNT][CONFIG_TEXT_CAPACITY];
    int64_t passiveLevels[CATDATA_PASSIVE_COUNT];
} StrayConfig;

extern StrayConfig g_config;

void LoadStrayConfig(void);
