#include "spawn_coordinator.h"
#include "battle_observer.h"
#include "config.h"
#include "mod_state.h"
#include "spawn.h"
#include "string_utils.h"

static HANDLE g_spawnCoordinatorThread = NULL;
static HANDLE g_spawnCoordinatorStopEvent = NULL;
static volatile LONG g_spawnCoordinatorStarted = 0;

static int IsSceneActiveByName(const char* sceneName)
{
    UINT_PTR gameBase;
    MewDirector** singletonSlot;
    MewDirector* mewDirector;
    void** iterator;

    if (!sceneName || sceneName[0] == '\0' || !g_mj.GetGameBase)
    {
        return 0;
    }

    gameBase = g_mj.GetGameBase();

    if (!gameBase)
    {
        return 0;
    }

    singletonSlot = (MewDirector**)(gameBase + (UINT_PTR)DATAOFF_MEWDIRECTOR_SINGLETON);
    mewDirector = singletonSlot ? *singletonSlot : NULL;

    if (!mewDirector || !mewDirector->director)
    {
        return 0;
    }

    for (iterator = mewDirector->director->scenes.begin; iterator && iterator < mewDirector->director->scenes.end; ++iterator)
    {
        Scene* candidate;

        candidate = (Scene*)(*iterator);

        if (candidate && StringEqualsLiteral(&candidate->name, sceneName))
        {
            return 1;
        }
    }

    return 0;
}

static DWORD WINAPI SpawnCoordinatorThreadProc(LPVOID parameter)
{
    HANDLE stopEvent;
    int previousSpawnTriggerPressed;
    int previousBattleDumpPressed;
    int previousBattleActive;
    int previousHomeActive;
    int riftKittenAliveNearBattleEnd;
    int playerCatAliveNearBattleEnd;
    uint32_t riftKittenDeadPollStreak;
    uint32_t playerCatDeadPollStreak;
    int pendingReturnHomeSpawn;
    uint32_t statePollTicks;

    stopEvent = (HANDLE)parameter;
    previousSpawnTriggerPressed = 0;
    previousBattleDumpPressed = 0;
    previousBattleActive = 0;
    previousHomeActive = 0;
    riftKittenAliveNearBattleEnd = 0;
    playerCatAliveNearBattleEnd = 0;
    riftKittenDeadPollStreak = 0U;
    playerCatDeadPollStreak = 0U;
    pendingReturnHomeSpawn = 0;
    statePollTicks = 0U;

    /*
    * Okay... 16ms poll cadence (60hz)...
    * Scene/liveness checks are downsampled further (every 16 ticks/256ms/1 second) 
    * because they are heavier and obviously don't need per-frame precision!
    */
    while (WaitForSingleObject(stopEvent, 16U) == WAIT_TIMEOUT)
    {
        int spawnTriggerPressed;
        int battleDumpPressed;

        spawnTriggerPressed = ((GetAsyncKeyState(g_config.spawnTriggerVk) & 0x8000) != 0);
        battleDumpPressed = ((GetAsyncKeyState(g_config.battleDumpVk) & 0x8000) != 0);
        ++statePollTicks;

        if (spawnTriggerPressed && !previousSpawnTriggerPressed)
        {
            if (g_mj.Log)
            {
                g_mj.Log(MOD_NAME, "Configured spawn trigger detected! Attempting stray spawn!");
            }

            SpawnConfiguredStrayAtHouse();
        }

        if (battleDumpPressed && !previousBattleDumpPressed)
        {
            DumpBattleEntityData();
        }

        if ((statePollTicks % 16U) == 0U)
        {
            int battleActive;
            int homeActive;

            battleActive = IsSceneActiveByName(g_config.battleSceneName);
            homeActive = IsSceneActiveByName(g_config.sceneName);

            if (battleActive && !previousBattleActive)
            {
                riftKittenAliveNearBattleEnd = 0;
                playerCatAliveNearBattleEnd = 0;
                riftKittenDeadPollStreak = 0U;
                playerCatDeadPollStreak = 0U;
            }

            if (battleActive)
            {
                int hasLivingRiftKitten;
                int hasLivingPlayerCat;

                hasLivingRiftKitten = 0;
                hasLivingPlayerCat = 0;

                /*
                * Game objects may be mid-transition during scene swaps; Guard
                * probing helpers so transient invalid pointers don't tear us down!
                */
                __try
                {
                    hasLivingRiftKitten = IsBattleEntityAliveById("RiftKitten");
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    hasLivingRiftKitten = 0;
                }
                __try
                {
                    hasLivingPlayerCat = IsAnyPlayerCatAlive();
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    hasLivingPlayerCat = 0;
                }

                /*
                * Debounce: Require several consecutive "dead" polls before we clear 
                * a previously observed alive state... (To avoid one-frame false negatives near battle teardown)
                */
                if (hasLivingRiftKitten)
                {
                    riftKittenAliveNearBattleEnd = 1;
                    riftKittenDeadPollStreak = 0U;
                }
                else if (riftKittenAliveNearBattleEnd)
                {
                    ++riftKittenDeadPollStreak;

                    if (riftKittenDeadPollStreak >= 4U)
                    {
                        riftKittenAliveNearBattleEnd = 0;
                    }
                }

                if (hasLivingPlayerCat)
                {
                    playerCatAliveNearBattleEnd = 1;
                    playerCatDeadPollStreak = 0U;
                }
                else if (playerCatAliveNearBattleEnd)
                {
                    ++playerCatDeadPollStreak;

                    if (playerCatDeadPollStreak >= 4U)
                    {
                        playerCatAliveNearBattleEnd = 0;
                    }
                }
            }

            if (previousBattleActive && !battleActive)
            {
                if (riftKittenAliveNearBattleEnd && playerCatAliveNearBattleEnd)
                {
                    pendingReturnHomeSpawn = 1;

                    if (g_mj.Log)
                    {
                        g_mj.Log(MOD_NAME, "Battle ended with RiftKitten and at least one PlayerCat alive! Waiting to return home to spawn stray!");
                    }
                }
                else if (riftKittenAliveNearBattleEnd && g_mj.Log)
                {
                    g_mj.Log(MOD_NAME, "Battle ended without any living PlayerCat... Skipping stray spawn (Defeat)!");
                }

                riftKittenAliveNearBattleEnd = 0;
                playerCatAliveNearBattleEnd = 0;
                riftKittenDeadPollStreak = 0U;
                playerCatDeadPollStreak = 0U;
            }

            if (pendingReturnHomeSpawn && !battleActive && homeActive)
            {
                if (g_mj.Log)
                {
                    g_mj.Log(MOD_NAME, "Returned home after battle with living RiftKitten! Triggered stray spawn!");
                }

                SpawnConfiguredStrayAtHouse();
                pendingReturnHomeSpawn = 0;
            }

            previousBattleActive = battleActive;
            previousHomeActive = homeActive;
        }

        previousSpawnTriggerPressed = spawnTriggerPressed;
        previousBattleDumpPressed = battleDumpPressed;
    }

    return 0U;
}

void StartSpawnCoordinator(void)
{
    if (InterlockedCompareExchange(&g_spawnCoordinatorStarted, 1, 0) != 0)
    {
        return;
    }

    g_spawnCoordinatorStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    if (!g_spawnCoordinatorStopEvent)
    {
        InterlockedExchange(&g_spawnCoordinatorStarted, 0);
        return;
    }

    g_spawnCoordinatorThread = CreateThread(NULL, 0U, SpawnCoordinatorThreadProc, g_spawnCoordinatorStopEvent, 0U, NULL);

    if (g_spawnCoordinatorThread && g_mj.Log)
    {
        g_mj.Log(MOD_NAME, "Spawn coordinator thread started!");
    }

    if (!g_spawnCoordinatorThread)
    {
        CloseHandle(g_spawnCoordinatorStopEvent);
        g_spawnCoordinatorStopEvent = NULL;
        InterlockedExchange(&g_spawnCoordinatorStarted, 0);
    }
}

void StopSpawnCoordinator(void)
{
    if (g_spawnCoordinatorStopEvent)
    {
        SetEvent(g_spawnCoordinatorStopEvent);
    }

    if (g_spawnCoordinatorThread)
    {
        WaitForSingleObject(g_spawnCoordinatorThread, 1000U);
        CloseHandle(g_spawnCoordinatorThread);
        g_spawnCoordinatorThread = NULL;
    }

    if (g_spawnCoordinatorStopEvent)
    {
        CloseHandle(g_spawnCoordinatorStopEvent);
        g_spawnCoordinatorStopEvent = NULL;
    }

    InterlockedExchange(&g_spawnCoordinatorStarted, 0);
}