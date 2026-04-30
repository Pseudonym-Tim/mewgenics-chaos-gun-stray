#include "config.h"
#include "spawn_coordinator.h"
#include "mod_state.h"
#include "game_runtime_types.h"

MewjectorAPI g_mj;
HMODULE g_hModule = NULL;
fn_scene_create_entity g_origSceneCreateEntity = NULL;

/*
* Hook entry for Scene CreateEntity (RVA_SCENE_CREATE_ENTITY)...
*
* - The game creates entities frequently while scene data is being assembled...
* - We use this as a reliable "game is running and scenes exist" signal...
* - The actual spawn logic is deferred to the coordinator thread so we avoid heavy work directly inside the hook chain...
*/
static Entity* __fastcall HookSceneCreateEntity(Scene* scene)
{
    Entity* result;

    if (!g_origSceneCreateEntity)
    {
        return NULL;
    }

    result = g_origSceneCreateEntity(scene);
    StartSpawnCoordinator();
    return result;
}

static void Initialize(void)
{
    void* trampoline;

    if (!MJ_Resolve(&g_mj))
    {
        return;
    }

    trampoline = NULL;
    LoadStrayConfig();

    // Install chainable hook at Scene CreateEntity...
    if (g_mj.InstallHook(RVA_SCENE_CREATE_ENTITY, SCENE_CREATE_ENTITY_STOLEN_BYTES, (void*)HookSceneCreateEntity, &trampoline, 32, MOD_NAME))
    {
        g_origSceneCreateEntity = (fn_scene_create_entity)trampoline;

        if (g_mj.Log)
        {
            g_mj.Log(MOD_NAME, "Loaded configurable stray coordinator mod. Waiting for Scene CreateEntity to start the spawn coordinator!");
        }
    }
    else if (g_mj.Log)
    {
        g_mj.Log(MOD_NAME, "Failed to hook Scene CreateEntity!");
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        Initialize();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        StopSpawnCoordinator();
    }

    return TRUE;
}
