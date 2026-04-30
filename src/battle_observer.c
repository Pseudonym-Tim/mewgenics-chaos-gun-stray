#include "battle_observer.h"
#include "config.h"
#include "mod_state.h"
#include "game_runtime_types.h"
#include "string_utils.h"

#define OWNER_OFFSET_SCAN_MAX 0x100U // Max bytes scanned while inferring component owner pointer...
#define OWNER_OFFSET_SCAN_STEP 8U // Pointer-aligned scan stride for owner-offset probing...
#define CHARACTER_CURRENT_HP_OFFSET 0x4B0U // Character component current HP...
#define CHARACTER_TEMPLATE_POINTER_OFFSET 0x240U // Character component field: pointer to template/id-bearing object...
#define NARROWSTRING_INLINE_BUF_OFFSET 0x0U // (NarrowString inline buffer starts at struct base)...
#define NARROWSTRING_HEAP_PTR_OFFSET 0x0U // (NarrowString heap pointer overlays inline buffer in the storage union)...
#define NARROWSTRING_SIZE_OFFSET 0x10U // (NarrowString size byte offset in reversed runtime layout)...
#define NARROWSTRING_CAPACITY_OFFSET 0x18U // (NarrowString capacity byte offset in reversed runtime layout)...

/*
* Grab some important info on battle entities!
* To do this we use a hybrid strategy:
* - Fixed offsets where we have stable reversed evidence...
* - Bounded probing where it's noisier...
*/

typedef struct EntityComponentInfo
{
    void* entity;
    void* preferredIdSource;
    void* characterComponent;
    int hasTemplateIdSource;
    char entityIdText[64];
    char components[512U];
    uint32_t componentCount;
} EntityComponentInfo;

static Scene* FindBattleScene(void)
{
    UINT_PTR gameBase;
    MewDirector** singletonSlot;
    MewDirector* mewDirector;
    void** iterator;

    if (!g_mj.GetGameBase)
    {
        return NULL;
    }

    gameBase = g_mj.GetGameBase();

    if (!gameBase)
    {
        return NULL;
    }

    singletonSlot = (MewDirector**)(gameBase + (UINT_PTR)DATAOFF_MEWDIRECTOR_SINGLETON);
    mewDirector = singletonSlot ? *singletonSlot : NULL;

    if (!mewDirector || !mewDirector->director)
    {
        return NULL;
    }

    for (iterator = mewDirector->director->scenes.begin; iterator && iterator < mewDirector->director->scenes.end; ++iterator)
    {
        Scene* candidate;

        candidate = (Scene*)(*iterator);

        if (candidate && StringEqualsLiteral(&candidate->name, g_config.battleSceneName))
        {
            return candidate;
        }
    }

    return NULL;
}

static int SafeReadPointer(const void* address, void** outValue)
{
    __try
    {
        if (!address || !outValue)
        {
            return 0;
        }

        *outValue = *(void* const*)address;
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static int TryGetObjectTypeName(Component* object, char* outText, size_t outTextSize)
{
    NarrowString typeName;
    const char* typeNameData;
    UINT_PTR gameBase;
    fn_destroy_narrow_string destroyNarrowString;

    if (!outText || outTextSize == 0U)
    {
        return 0;
    }

    outText[0] = '\0';

    __try
    {
        if (!object || !object->vtable || !object->vtable->GetObjectTypeSTR)
        {
            return 0;
        }

        memset(&typeName, 0, sizeof(typeName));
        object->vtable->GetObjectTypeSTR(object, &typeName);
        typeNameData = GetNarrowStringData(&typeName);

        if (!typeNameData || typeNameData[0] == '\0')
        {
            return 0;
        }

        _snprintf_s(outText, outTextSize, _TRUNCATE, "%s", typeNameData);

        gameBase = g_mj.GetGameBase ? g_mj.GetGameBase() : 0U;

        if (gameBase)
        {
            destroyNarrowString = (fn_destroy_narrow_string)(gameBase + (UINT_PTR)RVA_DESTROY_NARROW_STRING);
            destroyNarrowString(&typeName);
        }

        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        outText[0] = '\0';
        return 0;
    }
}

static int SafeReadU64(const void* address, uint64_t* outValue)
{
    __try
    {
        if (!address || !outValue)
        {
            return 0;
        }

        *outValue = *(const uint64_t*)address;
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static int SafeReadI32(const void* address, int32_t* outValue)
{
    __try
    {
        if (!address || !outValue)
        {
            return 0;
        }

        *outValue = *(const int32_t*)address;
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static int SafeReadBytes(const void* address, char* outBytes, size_t byteCount)
{
    size_t i;

    if (!address || !outBytes || byteCount == 0U)
    {
        return 0;
    }

    __try
    {
        for (i = 0U; i < byteCount; ++i)
        {
            outBytes[i] = ((const char*)address)[i];
        }

        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static int FindEntityIndex(void** entityData, uint32_t entityCount, void* value)
{
    uint32_t i;

    if (!entityData)
    {
        return -1;
    }

    for (i = 0U; i < entityCount; ++i)
    {
        if (entityData[i] == value)
        {
            return (int)i;
        }
    }

    return -1;
}

static uint32_t DetectComponentOwnerOffset(void** entitiesData, uint32_t entityCount, void** componentData, uint32_t componentCount)
{
    uint32_t bestOffset;
    uint32_t bestMatches;
    uint32_t offset;

    if (!entitiesData || entityCount == 0U || !componentData || componentCount == 0U)
    {
        return 0U;
    }

    bestOffset = 0U;
    bestMatches = 0U;

    /*
    * Infer "component -> owner entity*" field by voting on the offset that
    * produces the most pointers matching known scene entities... yeah...
    */
    for (offset = 0U; offset <= OWNER_OFFSET_SCAN_MAX; offset += OWNER_OFFSET_SCAN_STEP)
    {
        uint32_t matches;
        uint32_t i;

        matches = 0U;

        for (i = 0U; i < componentCount; ++i)
        {
            void* component;
            void* maybeOwner;
            const uint8_t* probeAddress;

            component = componentData[i];

            if (!component)
            {
                continue;
            }

            probeAddress = (const uint8_t*)component + offset;

            if (!SafeReadPointer(probeAddress, &maybeOwner))
            {
                continue;
            }

            if (FindEntityIndex(entitiesData, entityCount, maybeOwner) >= 0)
            {
                ++matches;
            }
        }

        if (matches > bestMatches)
        {
            bestMatches = matches;
            bestOffset = offset;
        }
    }

    if (bestMatches == 0U)
    {
        return 0U;
    }

    return bestOffset;
}

static void AppendComponentName(char* destination, size_t destinationSize, const char* componentName)
{
    size_t currentLength;

    if (!destination || destinationSize == 0U || !componentName || componentName[0] == '\0')
    {
        return;
    }

    currentLength = strlen(destination);

    if (currentLength == 0U)
    {
        _snprintf_s(destination, destinationSize, _TRUNCATE, "%s", componentName);
    }
    else
    {
        _snprintf_s(destination + currentLength, destinationSize - currentLength, _TRUNCATE, ", %s", componentName);
    }
}

static int IsLikelyEntityIdText(const char* text, size_t length)
{
    size_t i;
    int hasAlpha;

    if (!text || length < 3U || length > 48U)
    {
        return 0;
    }

    hasAlpha = 0;

    for (i = 0U; i < length; ++i)
    {
        unsigned char c;

        c = (unsigned char)text[i];

        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
        {
            return 0;
        }

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
        {
            hasAlpha = 1;
        }
    }

    if (!hasAlpha)
    {
        return 0;
    }

    return 1;
}

static int TryExtractIdStringFromObject(void* object, char* outText, size_t outTextSize)
{
    uint32_t offset;

    if (!object || !outText || outTextSize == 0U)
    {
        return 0;
    }

    outText[0] = '\0';

    /*
    * Scan for NarrowString fields, keep only sane identifier-ish tokens. Best effort discovery...
    */
    for (offset = 0U; offset <= 0x300U; offset += 8U)
    {
        const uint8_t* base;
        uint64_t sizeValue;
        uint64_t capacityValue;
        const char* rawData;
        char temp[64];

        base = (const uint8_t*)object + offset;

        if (!SafeReadU64(base + NARROWSTRING_SIZE_OFFSET, &sizeValue) || !SafeReadU64(base + NARROWSTRING_CAPACITY_OFFSET, &capacityValue))
        {
            continue;
        }

        if (sizeValue == 0ULL || sizeValue >= sizeof(temp))
        {
            continue;
        }

        if (capacityValue > 15ULL)
        {
            void* heapPtr;

            if (!SafeReadPointer(base + NARROWSTRING_HEAP_PTR_OFFSET, &heapPtr) || !heapPtr)
            {
                continue;
            }

            rawData = (const char*)heapPtr;
        }
        else
        {
            rawData = (const char*)(base + NARROWSTRING_INLINE_BUF_OFFSET);
        }

        if (!SafeReadBytes(rawData, temp, (size_t)sizeValue))
        {
            continue;
        }

        temp[sizeValue] = '\0';

        if (!IsLikelyEntityIdText(temp, (size_t)sizeValue))
        {
            continue;
        }

        // Ensure we ignore some stupid garbage we don't care about... Ugh...
        if (_stricmp(temp, "Transform") == 0 || _stricmp(temp, "RendererIso") == 0 || _stricmp(temp, "MaskedRendererIso") == 0 || _stricmp(temp, "graphic") == 0)
        {
            continue;
        }

        _snprintf_s(outText, outTextSize, _TRUNCATE, "%s", temp);
        return 1;
    }

    return 0;
}

static int TryExtractCharacterHP(void* characterComponent, int32_t* outCurrentHp)
{
    int32_t value;

    if (!characterComponent || !outCurrentHp)
    {
        return 0;
    }

    if (!SafeReadI32((const uint8_t*)characterComponent + CHARACTER_CURRENT_HP_OFFSET, &value))
    {
        return 0;
    }

    if (value < 0 || value > 100000)
    {
        return 0;
    }

    *outCurrentHp = value;
    return 1;
}

static EntityComponentInfo* CollectEntityComponentRecords(Scene* battleScene, uint32_t* outEntityCount, uint32_t* outOwnerOffset, int includeComponentNames)
{
    void** entitiesData;
    uint32_t entityCount;
    void** componentData;
    uint32_t componentCount;
    EntityComponentInfo* entityRecords;
    uint32_t ownerOffset;
    uint32_t i;

    if (!battleScene || !outEntityCount || !outOwnerOffset)
    {
        return NULL;
    }

    entitiesData = battleScene->entities.data;
    entityCount = battleScene->entities.size;
    componentData = (battleScene->componentLists ? battleScene->componentLists->data : NULL);
    componentCount = (battleScene->componentLists ? battleScene->componentLists->size : 0U);

    if (!entitiesData || entityCount == 0U)
    {
        return NULL;
    }

    entityRecords = (EntityComponentInfo*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(EntityComponentInfo) * (size_t)entityCount);

    if (!entityRecords)
    {
        return NULL;
    }

    for (i = 0U; i < entityCount; ++i)
    {
        entityRecords[i].entity = entitiesData[i];
        entityRecords[i].preferredIdSource = NULL;
        entityRecords[i].hasTemplateIdSource = 0;
        entityRecords[i].characterComponent = NULL;
        entityRecords[i].entityIdText[0] = '\0';
    }

    ownerOffset = DetectComponentOwnerOffset(entitiesData, entityCount, componentData, componentCount);

    if (componentData)
    {
        for (i = 0U; i < componentCount; ++i)
        {
            Component* component;
            void* owner;
            char componentName[64];
            int entityIndex;

            component = (Component*)componentData[i];

            if (!component)
            {
                continue;
            }

            if (!TryGetObjectTypeName(component, componentName, sizeof(componentName)))
            {
                if (!includeComponentNames)
                {
                    continue;
                }

                _snprintf_s(componentName, sizeof(componentName), _TRUNCATE, "UnknownComponent");
            }

            owner = NULL;

            if (ownerOffset != 0U)
            {
                SafeReadPointer((const uint8_t*)component + ownerOffset, &owner);
            }

            entityIndex = FindEntityIndex(entitiesData, entityCount, owner);

            if (entityIndex < 0)
            {
                if (includeComponentNames && g_mj.Log)
                {
                    g_mj.Log(MOD_NAME, "Battle component (Unowned): %s (%p)", componentName, component);
                }

                continue;
            }

            if (includeComponentNames)
            {
                AppendComponentName(entityRecords[entityIndex].components, sizeof(entityRecords[entityIndex].components), componentName);
                ++entityRecords[entityIndex].componentCount;
            }

            // Character component provides template pointer + HP source fields!
            if (_stricmp(componentName, "Character") == 0)
            {
                void* templatePointer;

                entityRecords[entityIndex].characterComponent = component;
                templatePointer = NULL;

                if (SafeReadPointer((const uint8_t*)component + CHARACTER_TEMPLATE_POINTER_OFFSET, &templatePointer) && templatePointer)
                {
                    entityRecords[entityIndex].preferredIdSource = templatePointer;
                    entityRecords[entityIndex].hasTemplateIdSource = 1;
                    TryExtractIdStringFromObject(templatePointer, entityRecords[entityIndex].entityIdText, sizeof(entityRecords[entityIndex].entityIdText));
                }
            }
        }
    }

    *outEntityCount = entityCount;
    *outOwnerOffset = ownerOffset;
    return entityRecords;
}

static int TryResolveEntityCurrentHP(const EntityComponentInfo* entityInfo, int32_t* outCurrentHp, uint32_t* outInferredOffset, const char** outHpSource)
{
    int32_t currentHp;
    uint32_t inferredOffset;
    const char* hpSource;
    int hasHp;

    if (!entityInfo || !entityInfo->characterComponent || !outCurrentHp)
    {
        return 0;
    }

    hasHp = 0;
    inferredOffset = 0U;
    hpSource = "unknown";

    if (TryExtractCharacterHP(entityInfo->characterComponent, &currentHp))
    {
        hasHp = 1;
        inferredOffset = CHARACTER_CURRENT_HP_OFFSET;
        hpSource = "character_known_offset_0x4B0";
    }

    if (!hasHp)
    {
        return 0;
    }

    *outCurrentHp = currentHp;

    if (outInferredOffset)
    {
        *outInferredOffset = inferredOffset;
    }

    if (outHpSource)
    {
        *outHpSource = hpSource;
    }

    return 1;
}

int DumpBattleEntityData(void)
{
    Scene* battleScene;
    uint32_t ownerOffset;
    EntityComponentInfo* entityRecords;
    uint32_t entityCount;
    uint32_t i;

    if (!g_mj.Log)
    {
        return 0;
    }

    battleScene = FindBattleScene();

    if (!battleScene)
    {
        g_mj.Log(MOD_NAME, "Battle observer: Scene '%s' not found!", g_config.battleSceneName);
        return 0;
    }

    entityRecords = CollectEntityComponentRecords(battleScene, &entityCount, &ownerOffset, 1);

    if (!entityRecords)
    {
        g_mj.Log(MOD_NAME, "Battle observer: Scene '%s' has no entities or allocation failed!", g_config.battleSceneName);
        return 0;
    }

    g_mj.Log(MOD_NAME, "Battle observer: Scene=%p '%s' entities=%u components=%u ownerOffset=0x%X", battleScene, g_config.battleSceneName, entityCount, (battleScene->componentLists ? battleScene->componentLists->size : 0U), ownerOffset);

    for (i = 0U; i < entityCount; ++i)
    {
        int32_t currentHp;
        uint32_t inferredOffset;
        const char* hpSource;
        int hasHp;

        if (!entityRecords[i].characterComponent)
        {
            continue;
        }

        hasHp = TryResolveEntityCurrentHP(&entityRecords[i], &currentHp, &inferredOffset, &hpSource);

        if (entityRecords[i].componentCount > 0U)
        {
            if (entityRecords[i].entityIdText[0] != '\0')
            {
                if (hasHp)
                {
                    g_mj.Log(MOD_NAME, "Entity[id=%s,index=%u,hp=%d,src=%s,off=0x%X]: %s", entityRecords[i].entityIdText, i, currentHp, hpSource, inferredOffset, entityRecords[i].components);
                }
                else
                {
                    g_mj.Log(MOD_NAME, "Entity[id=%s,index=%u,hp=?]: %s", entityRecords[i].entityIdText, i, entityRecords[i].components);
                }
            }
            else
            {
                if (hasHp)
                {
                    g_mj.Log(MOD_NAME, "Entity[index=%u,hp=%d,src=%s,off=0x%X]: %s", i, currentHp, hpSource, inferredOffset, entityRecords[i].components);
                }
                else
                {
                    g_mj.Log(MOD_NAME, "Entity[index=%u,hp=?]: %s", i, entityRecords[i].components);
                }
            }
        }
        else
        {
            if (entityRecords[i].entityIdText[0] != '\0')
            {
                if (hasHp)
                {
                    g_mj.Log(MOD_NAME, "Entity[id=%s,index=%u,hp=%d,src=%s,off=0x%X]: (No linked components)", entityRecords[i].entityIdText, i, currentHp, hpSource, inferredOffset);
                }
                else
                {
                    g_mj.Log(MOD_NAME, "Entity[id=%s,index=%u,hp=?]: (No linked components)", entityRecords[i].entityIdText, i);
                }
            }
            else
            {
                if (hasHp)
                {
                    g_mj.Log(MOD_NAME, "Entity[index=%u,hp=%d,src=%s,off=0x%X]: (No linked components)", i, currentHp, hpSource, inferredOffset);
                }
                else
                {
                    g_mj.Log(MOD_NAME, "Entity[index=%u,hp=?]: (No linked components)", i);
                }
            }
        }
    }

    HeapFree(GetProcessHeap(), 0U, entityRecords);
    return 1;
}

int IsBattleEntityAliveById(const char* entityId)
{
    Scene* battleScene;
    uint32_t ownerOffset;
    EntityComponentInfo* entityRecords;
    uint32_t entityCount;
    uint32_t i;
    int isAlive;

    entityRecords = NULL;

    if (!entityId || entityId[0] == '\0')
    {
        return 0;
    }

    __try
    {
        battleScene = FindBattleScene();

        if (!battleScene)
        {
            return 0;
        }

        entityRecords = CollectEntityComponentRecords(battleScene, &entityCount, &ownerOffset, 0);

        /*
        * If owner offset can't be inferred, avoid guessing and just report "not alive" for safety...
        */
        if (!entityRecords || ownerOffset == 0U)
        {
            if (entityRecords)
            {
                HeapFree(GetProcessHeap(), 0U, entityRecords);
            }

            return 0;
        }

        isAlive = 0;

        for (i = 0U; i < entityCount; ++i)
        {
            int32_t currentHp;
            int hasHp;

            if (!entityRecords[i].characterComponent)
            {
                continue;
            }

            if (_stricmp(entityRecords[i].entityIdText, entityId) != 0)
            {
                continue;
            }

            hasHp = TryResolveEntityCurrentHP(&entityRecords[i], &currentHp, NULL, NULL);

            if (hasHp && currentHp > 0)
            {
                isAlive = 1;
                break;
            }
        }

        HeapFree(GetProcessHeap(), 0U, entityRecords);
        return isAlive;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (entityRecords)
        {
            HeapFree(GetProcessHeap(), 0U, entityRecords);
        }

        return 0;
    }
}

int IsAnyPlayerCatAlive(void)
{
    Scene* battleScene;
    uint32_t ownerOffset;
    EntityComponentInfo* entityRecords;
    uint32_t entityCount;
    uint32_t i;
    int isAlive;

    entityRecords = NULL;

    __try
    {
        battleScene = FindBattleScene();

        if (!battleScene)
        {
            return 0;
        }

        entityRecords = CollectEntityComponentRecords(battleScene, &entityCount, &ownerOffset, 0);

        /*
        * If owner offset can't be inferred, avoid guessing and just report "not alive" for safety...
        */
        if (!entityRecords || ownerOffset == 0U)
        {
            if (entityRecords)
            {
                HeapFree(GetProcessHeap(), 0U, entityRecords);
            }

            return 0;
        }

        isAlive = 0;

        for (i = 0U; i < entityCount; ++i)
        {
            int32_t currentHp;
            int hasHp;

            if (!entityRecords[i].characterComponent)
            {
                continue;
            }

            if (_stricmp(entityRecords[i].entityIdText, "PlayerCat") != 0)
            {
                continue;
            }

            hasHp = TryResolveEntityCurrentHP(&entityRecords[i], &currentHp, NULL, NULL);

            if (hasHp && currentHp > 0)
            {
                isAlive = 1;
                break;
            }
        }

        HeapFree(GetProcessHeap(), 0U, entityRecords);
        return isAlive;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (entityRecords)
        {
            HeapFree(GetProcessHeap(), 0U, entityRecords);
        }

        return 0;
    }
}
