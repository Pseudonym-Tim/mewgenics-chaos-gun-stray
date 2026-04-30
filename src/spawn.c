#include "spawn.h"
#include "cat_customization.h"
#include "config.h"
#include "mod_state.h"
#include "string_utils.h"

static volatile LONG g_isSpawning = 0;

static int SpawnGuardBegin(void)
{
    return InterlockedCompareExchange(&g_isSpawning, 1, 0) == 0;
}

static void SpawnGuardEnd(void)
{
    InterlockedExchange(&g_isSpawning, 0);
}

static MewDirector* GetMewDirector(void)
{
    UINT_PTR gameBase;
    MewDirector** singletonSlot;

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
    return *singletonSlot;
}

static Scene* FindConfiguredScene(MewDirector* mewDirector)
{
    void** iterator;

    if (!mewDirector || !mewDirector->director)
    {
        return NULL;
    }

    for (iterator = mewDirector->director->scenes.begin; iterator && iterator < mewDirector->director->scenes.end; ++iterator)
    {
        Scene* candidate;

        candidate = (Scene*)(*iterator);

        if (candidate && StringEqualsLiteral(&candidate->name, g_config.sceneName))
        {
            return candidate;
        }
    }

    return NULL;
}

static Component* FindConfiguredHouseComponent(Scene* houseScene)
{
    uint32_t index;

    if (!houseScene || !houseScene->componentLists || !houseScene->componentLists->data)
    {
        return NULL;
    }

    for (index = 0U; index < houseScene->componentLists->size; ++index)
    {
        Component* candidate;
        NarrowString typeName;

        candidate = (Component*)houseScene->componentLists->data[index];

        if (!candidate || !candidate->vtable || !candidate->vtable->GetObjectTypeSTR)
        {
            continue;
        }

        memset(&typeName, 0, sizeof(typeName));
        candidate->vtable->GetObjectTypeSTR(candidate, &typeName);

        if (StringEqualsLiteral(&typeName, g_config.houseComponentType))
        {
            return candidate;
        }
    }

    return NULL;
}

int SpawnConfiguredStrayAtHouse(void)
{
    UINT_PTR gameBase;
    MewDirector* mewDirector;
    Scene* houseScene;
    Component* houseComponent;
    fn_create_stray_catdata createStrayCatData;
    fn_scene_create_entity sceneCreateEntity;
    fn_scene_create_housecat_i64 sceneCreateHouseCat;
    CatData* cat;
    Entity* entity;
    HouseCat* houseCat;
    void* houseCatTransform;

    if (!SpawnGuardBegin())
    {
        return 0;
    }

    if (!g_mj.GetGameBase)
    {
        SpawnGuardEnd();
        return 0;
    }

    gameBase = g_mj.GetGameBase();
    mewDirector = GetMewDirector();
    houseScene = FindConfiguredScene(mewDirector);

    if (!gameBase || !mewDirector || !houseScene || houseScene->doingSceneDestruction)
    {
        SpawnGuardEnd();
        return 0;
    }

    houseComponent = FindConfiguredHouseComponent(houseScene);

    if (!houseComponent || !mewDirector->cats)
    {
        SpawnGuardEnd();
        return 0;
    }

    createStrayCatData = (fn_create_stray_catdata)(gameBase + (UINT_PTR)RVA_CREATE_STRAY_CATDATA);
    sceneCreateEntity = (fn_scene_create_entity)(gameBase + (UINT_PTR)RVA_SCENE_CREATE_ENTITY);
    sceneCreateHouseCat = (fn_scene_create_housecat_i64)(gameBase + (UINT_PTR)RVA_SCENE_CREATE_HOUSECAT_I64);
    cat = createStrayCatData(mewDirector->cats, NULL, g_config.spawnSex);

    if (!cat)
    {
        SpawnGuardEnd();
        return 0;
    }

    ApplyNativeCustomCatTemplate(cat);
    ApplySpecialStrayMetadata(cat);
    ApplyConfiguredStatsAbilitiesAndPassives(cat);
    entity = sceneCreateEntity(houseScene);

    if (!entity)
    {
        SpawnGuardEnd();
        return 0;
    }

    houseCat = sceneCreateHouseCat(houseScene, entity, &cat->sqlKey);

    if (!houseCat)
    {
        SpawnGuardEnd();
        return 0;
    }

    /*
    * houseCat + 0x40: transform component used for world position...
    * transform + 0x80/0x88: X/Y position copied from the house anchor component...
    * transform + 0x90: Z position...
    */
    houseCatTransform = *(void**)((uint8_t*)houseCat + 0x40U);

    if (houseCatTransform)
    {
        *(uint64_t*)((uint8_t*)houseCatTransform + 0x80U) = *(uint64_t*)((uint8_t*)houseComponent + 0x90U);
        *(uint64_t*)((uint8_t*)houseCatTransform + 0x88U) = *(uint64_t*)((uint8_t*)houseComponent + 0x98U);
        *(uint64_t*)((uint8_t*)houseCatTransform + 0x90U) = 0ULL; // (Reset to 0 to ensure the stray is on the expected plane)...
    }

    if (g_mj.Log)
    {
        g_mj.Log(MOD_NAME, "Spawned configured stray with SQL key %lld", (long long)cat->sqlKey);
    }

    SpawnGuardEnd();
    return 1;
}