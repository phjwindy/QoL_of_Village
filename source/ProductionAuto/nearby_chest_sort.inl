// ---- nearby chest quick-stack ---------------------------------------------
//
// The original world "Use Item" action is controller Y / Left Shift and can
// place the held item, so it is deliberately not used as this module's hotkey.
// Instead, press controller D-pad Up for the quick-stack sort.  (The F8 key
// trigger was removed at the user's request; D-pad Up remains the only
// entry for this feature.)
// D-pad Up is routed by the game as CMD_UNIT_ACTION_UP (0x3f8); this module
// consumes that one normal-world command edge before starting a sort. Menu
// navigation uses separate task/input routing and remains untouched. Input is sampled
// only from CState_Main@CTask_Livelihood::update and still
// passes CCom_Input's focus/control gate. Chest/talk/menu states suspend that
// update, so the world hotkey cannot race menu input.
//
// Discovery uses the live map's native spatial index and the same callback
// implementation used by MapSearchStatusInRect_Gimmick.  No registered-object
// pointers are cached.  Transfers only merge into already-existing matching
// stacks.  Their counts are changed with the native CItemStatus clamp helper.
// Player slots are committed with the same intrusive-reference pattern used by
// the vanilla chest-close batch, followed by exactly one inventory/UI refresh
// for the whole batch.  Each operation is verified and can be rolled back
// before another chest is touched. If several existing stacks match, the
// fullest available stack is selected first; counts are compared only after a
// second match is found. Empty
// chest slots are deliberately left alone: quick-stack is classification, not
// a request to create a new category in an arbitrary box.

// auto_pet.inl is included after this dispatcher.  These overload declarations
// keep the dispatcher entry snapshot complete without creating a second hook.
static u64 AutoPetCaptureLoadReadyGeneration();
static u64 AutoPetCaptureLoadInFlightGeneration();
static u64 AutoPetBeginMainCallback();
static void AutoPetCompleteMainCallback(u64 callbackSequence);
static void AutoPetOnMainThreadUpdate(
    void* state, u64 readyGenerationAtCallbackEntry,
    u64 pendingDailyGenerationAtCallbackEntry,
    u64 reloadInvocationGenerationAtCallbackEntry,
    u64 loadReadyGenerationAtCallbackEntry,
    u64 loadInFlightGenerationAtCallbackEntry,
    u64 callbackSequence);

// RVA_NEARBY_* 常量已在 version_manifest.h 中定义为宏，此处不再重复定义

static constexpr uintptr_t NEARBY_STATE_INPUT_OFFSET = 0x1a8;
static constexpr uintptr_t NEARBY_INPUT_ENABLE_WORD_OFFSET = 0x1ac;
static constexpr uintptr_t NEARBY_ROOT_PLAYER_OFFSET = 0x208;
static constexpr uintptr_t NEARBY_ROOT_MAP_OWNER_OFFSET = 0x268;
static constexpr uintptr_t NEARBY_MAP_INFO_OFFSET = 0x0d8;
static constexpr uintptr_t NEARBY_MAP_SPATIAL_OWNER_OFFSET = 0x030;
static constexpr uintptr_t NEARBY_MAP_SPATIAL_INDEX_OFFSET = 0x6e0;
static constexpr uintptr_t NEARBY_PLAYER_OBJECT_STATUS_OFFSET = 0x32b8;
static constexpr uintptr_t NEARBY_PLAYER_OBJECT_LINK_A_OFFSET = 0x410;
static constexpr uintptr_t NEARBY_PLAYER_OBJECT_LINK_B_OFFSET = 0x008;
static constexpr uintptr_t NEARBY_PLAYER_OBJECT_ID_OFFSET = 0x028;
static constexpr uintptr_t NEARBY_PLAYER_ITEMS_OFFSET = 0x32c0;
static constexpr uintptr_t NEARBY_WORLD_OBJECT_POSITION_OFFSET = 0x230;
static constexpr uintptr_t NEARBY_WORLD_OBJECT_DIRTY_OFFSET = 0x300;
static constexpr uintptr_t NEARBY_STATUS_ITEMS_OFFSET = 0x2b8;
static constexpr uintptr_t NEARBY_ITEM_DATA_HOLDER_OFFSET = 0x240;
static constexpr uintptr_t NEARBY_ITEM_STACK_COUNT_OFFSET = 0x260;
static constexpr uintptr_t NEARBY_ITEM_RANK_OFFSET = 0x280;

static constexpr u64 NEARBY_DPAD_UP_COMMAND = 0x3f8;
static constexpr uintptr_t NEARBY_INPUT_EVENT_ACTIVE_OFFSET = 0x80;
static constexpr ULONGLONG NEARBY_MAIN_CALLBACK_GAP_MS = 100;
static constexpr float NEARBY_SORT_RADIUS = 720.0f; // 16 world-grid cells.
static constexpr size_t NEARBY_CHEST_SLOT_COUNT = 30;
static constexpr size_t NEARBY_MAX_CANDIDATES = 32;
static constexpr size_t NEARBY_MAX_SEARCH_RESULTS = 4096;
static constexpr size_t NEARBY_MAX_SEARCH_CAPACITY = 8192;

struct NearbyRect {
    float minimumX;
    float minimumY;
    float maximumX;
    float maximumY;
};

struct NearbyRawPointerVector {
    void** begin;
    void** end;
    void** capacity;
};

struct NearbySearchFunction {
    alignas(16) unsigned char storage[0x38];
    void* target;
};

static_assert(sizeof(NearbySearchFunction) == 0x40,
              "native spatial callback size changed");
static_assert(offsetof(NearbySearchFunction, target) == 0x38,
              "native spatial callback target offset changed");

struct NearbyCommandItem {
    void* item;
    unsigned char enabled;
    unsigned char padding09[3];
    int field0c;
    int state;
    unsigned char dirty;
    unsigned char padding15[3];
};

static_assert(sizeof(NearbyCommandItem) == 0x18,
              "native chest CommandItem layout changed");

struct NearbyItemInfo {
    u64 itemId;
    int stackCount;
    int rank;
};

struct NearbyChestCandidate {
    void* status;
    float positionX;
    float positionY;
    float distanceSquared;
};

struct NearbyDestinationPlan {
    void* item;
    size_t chestSlot;
    int beforeCount;
    int amount;
};

struct NearbyChestSlotSnapshot {
    void* item;
    NearbyItemInfo info;
    int available;
    bool valid;
    bool capacityKnown;
};

struct NearbyChestInventorySnapshot {
    void* status;
    NearbyChestSlotSnapshot slots[NEARBY_CHEST_SLOT_COUNT];
    bool valid;
};

struct NearbyPlayerSlotSnapshot {
    void* item;
    NearbyItemInfo info;
    bool valid;
};

struct NearbyTransferMetrics {
    u64 itemReads;
    u64 planCalls;
    u64 chestSlotVisits;
    u64 duplicateCountComparisons;
    u64 bestCountReplacements;
    u64 capacityCalls;
    u64 matchingTargets;
    u64 positiveCapacityTargets;
    u64 zeroCapacityNonstackableTargets;
    u64 zeroCapacityFullOrNonstackableTargets;
    u64 forbiddenCalls;
    LONGLONG planTicks;
    LONGLONG capacityTicks;
    LONGLONG forbiddenTicks;
};

using NearbyMainUpdateFunction = bool (__fastcall *)(void*, void*);
using NearbyInputActiveFunction = bool (__fastcall *)(void*);
using NearbyInputConsumeFunction = bool (__fastcall *)(void*, u64, void*);
using NearbyInputEventFunction = void* (__fastcall *)(void*, u64, void*);
using NearbySpatialSearchFunction = bool (__fastcall *)(
    void*, const NearbyRect*, void*, int, int);
using NearbyResolveWorldObjectFunction = void* (__fastcall *)(void*, u64);
using NearbyRefreshWorldObjectFunction = void (__fastcall *)(void*);
using NearbyRawVectorFreeFunction = void (__fastcall *)(void*, size_t);
using NearbyCommandCapacityFunction = int (__fastcall *)(NearbyCommandItem*);
using NearbyIntrusiveReleaseFunction = void (__fastcall *)(void**);
using NearbyItemAdjustFunction = void (__fastcall *)(void*, int);
using NearbyForbiddenItemFunction = bool (__fastcall *)(u64);
using NearbyPlayerCapacityFunction = int (__fastcall *)(void*);
using NearbyBatchRecalculateFunction = void (__fastcall *)(void*, bool);
using NearbyBatchUiRefreshFunction = void (__fastcall *)(void*, bool);
using NearbyBatchDirtyFunction = void (__fastcall *)(void*, int);

static std::atomic<NearbyMainUpdateFunction> g_nearbyOriginalMainUpdate{nullptr};
static void* g_nearbyMainTrampoline = nullptr;
static void* g_nearbyMainRelay = nullptr;  // v1.1.49: near relay for unload (H2)
static NearbyInputActiveFunction g_nearbyInputActive = nullptr;
static NearbyInputConsumeFunction g_nearbyInputConsume = nullptr;
static NearbyInputEventFunction g_nearbyInputEvent = nullptr;
static NearbySpatialSearchFunction g_nearbySpatialSearch = nullptr;
static NearbyResolveWorldObjectFunction g_nearbyResolveWorldObject = nullptr;
static NearbyRefreshWorldObjectFunction g_nearbyRefreshWorldObject = nullptr;
static NearbyRawVectorFreeFunction g_nearbyRawVectorFree = nullptr;
static NearbyCommandCapacityFunction g_nearbyCommandCapacity = nullptr;
static NearbyIntrusiveReleaseFunction g_nearbyIntrusiveRelease = nullptr;
static NearbyItemAdjustFunction g_nearbyItemAdjust = nullptr;
static NearbyForbiddenItemFunction g_nearbyForbiddenItem = nullptr;
static NearbyPlayerCapacityFunction g_nearbyPlayerCapacity = nullptr;
static NearbyBatchRecalculateFunction g_nearbyBatchRecalculate = nullptr;
static NearbyBatchUiRefreshFunction g_nearbyBatchUiRefresh = nullptr;
static NearbyBatchDirtyFunction g_nearbyBatchDirty = nullptr;
static LONGLONG g_nearbyPerformanceFrequency = 0;

static std::atomic<bool> g_nearbySortReady{false};
static std::atomic<bool> g_nearbySortEnabled{true};
static std::atomic<bool> g_nearbySortFaulted{false};
static std::atomic<void*> g_nearbyQuarantinedItem{nullptr};
static std::atomic<u64> g_nearbySortSequence{0};
static std::atomic<int> g_nearbyToastCode{0};
static std::atomic<int> g_nearbyToastStacks{0};
static std::atomic<int> g_nearbyToastItems{0};
static std::atomic<int> g_nearbyToastChests{0};
static std::atomic<bool> g_nearbyModalInputMaskFaultLogged{false};
static thread_local bool g_nearbySortRunning = false;
static thread_local bool g_nearbyDpadUpNeedsRelease = true;
static thread_local ULONGLONG g_nearbyLastMainUpdateAt = 0;
static thread_local void* g_nearbyLastInput = nullptr;

// v1.1.31: 帧率自适应节流——帧间隔突降时跳过辅助逻辑只调原生函数
// 维护 EMA 帧间隔基线，当前帧间隔比基线高 15ms 以上 → 降级
// 帧间隔恢复到基线 + 8ms 以内 → 退出降级
// 这样 60 帧玩家正常游玩不触发（17ms < 17+15=32ms），区域切换突降才触发
static thread_local double g_nearbyFrameBaselineMs = 0.0;  // EMA 基线
static thread_local ULONGLONG g_nearbyLastFrameTick = 0;
static thread_local bool g_nearbyThrottled = false;        // 是否处于降级模式
static constexpr double NEARBY_THROTTLE_SPIKE_MS = 15.0;  // 突降阈值：比基线高 15ms
static constexpr double NEARBY_THROTTLE_RECOVERY_MS = 8.0; // 恢复阈值：基线 + 8ms

enum NearbyToastCode : int {
    NEARBY_TOAST_NONE = 0,
    NEARBY_TOAST_SUCCESS = 1,
    NEARBY_TOAST_NO_CHESTS = 2,
    NEARBY_TOAST_NO_MATCH = 3,
    NEARBY_TOAST_STOPPED = 4,
};

static LONGLONG NearbyPerformanceCounter() {
    LARGE_INTEGER value = {};
    return QueryPerformanceCounter(&value) ? value.QuadPart : 0;
}

static double NearbyElapsedMilliseconds(LONGLONG begin, LONGLONG end) {
    if (g_nearbyPerformanceFrequency <= 0 || begin <= 0 || end < begin) return -1.0;
    return static_cast<double>(end - begin) * 1000.0 /
           static_cast<double>(g_nearbyPerformanceFrequency);
}

static double NearbyTicksToMilliseconds(LONGLONG ticks) {
    if (g_nearbyPerformanceFrequency <= 0 || ticks < 0) return -1.0;
    return static_cast<double>(ticks) * 1000.0 /
           static_cast<double>(g_nearbyPerformanceFrequency);
}

static void NearbyInitializeInputDescriptor(unsigned char* descriptor,
                                            bool consumeEdge) {
    memset(descriptor, 0, 0x60);
    *reinterpret_cast<unsigned short*>(descriptor + 0x20) = 0x0101;
    *reinterpret_cast<u32*>(descriptor + 0x24) = 0xff;
    descriptor[0x28] = consumeEdge ? 1 : 0;
    descriptor[0x2a] = consumeEdge ? 0 : 1;
    descriptor[0x2c] = 0;
    const uintptr_t base =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    *reinterpret_cast<void**>(descriptor + 0x30) =
        reinterpret_cast<void*>(base + RVA_NEARBY_INPUT_DESCRIPTOR_VTABLE);
    *reinterpret_cast<void**>(descriptor + 0x38) =
        reinterpret_cast<void*>(base + RVA_NEARBY_INPUT_DESCRIPTOR_DATA);
}

static bool NearbyInputCommandActive(void* input, u64 command) {
    alignas(16) unsigned char descriptor[0x60];
    NearbyInitializeInputDescriptor(descriptor, false);
    void* event = g_nearbyInputEvent(input, command, descriptor);
    return event && *reinterpret_cast<const unsigned char*>(
        reinterpret_cast<uintptr_t>(event) +
        NEARBY_INPUT_EVENT_ACTIVE_OFFSET) != 0;
}

static bool NearbyConsumeInputCommand(void* input, u64 command) {
    alignas(16) unsigned char descriptor[0x60];
    NearbyInitializeInputDescriptor(descriptor, true);
    // 0x7e0810 increments event+0xbc. Original edge readers later in this frame
    // increment it again and reject count > 1, which is the native consumption
    // mechanism used by routed commands. D-pad Up is intentionally replaced by
    // nearby quick-stack only in the normal-world Main input context.
    return g_nearbyInputConsume(input, command, descriptor);
}

static void NearbyDisarmCustomHotkey() {
    g_nearbyDpadUpNeedsRelease = true;
}

// VirtualQuery is unexpectedly expensive on this title's heap layout (about
// 2.4 ms per call in the v1.5.14 runtime trace).  A single quick-stack used to
// query the same committed heap regions thousands of times.  Cache only page
// mapping/protection for the duration of one synchronous main-thread batch;
// object identity, pointers and counts are still revalidated before every
// write.  The game cannot advance inventory ownership on this thread while the
// batch is running.
static constexpr size_t NEARBY_READABLE_REGION_CACHE_SIZE = 256;

struct NearbyReadableRegion {
    uintptr_t begin;
    uintptr_t end;
};

struct NearbyReadableRegionCache {
    NearbyReadableRegion regions[NEARBY_READABLE_REGION_CACHE_SIZE];
    size_t count;
    size_t replacement;
    size_t lastHit;
    u64 checks;
    u64 cacheHits;
    u64 virtualQueries;
    LONGLONG virtualQueryTicks;
    u64 liveVirtualQueries;
    LONGLONG liveVirtualQueryTicks;
    bool reuseEnabled;
};

static thread_local NearbyReadableRegionCache* g_nearbyReadableRegionCache = nullptr;

class NearbyReadableRegionCacheScope {
public:
    explicit NearbyReadableRegionCacheScope(NearbyReadableRegionCache* cache)
        : previous_(g_nearbyReadableRegionCache), active_(true) {
        if (cache) cache->reuseEnabled = true;
        g_nearbyReadableRegionCache = cache;
    }

    ~NearbyReadableRegionCacheScope() {
        Deactivate();
    }

    void Deactivate() {
        if (!active_) return;
        g_nearbyReadableRegionCache = previous_;
        active_ = false;
    }

    NearbyReadableRegionCacheScope(const NearbyReadableRegionCacheScope&) = delete;
    NearbyReadableRegionCacheScope& operator=(
        const NearbyReadableRegionCacheScope&) = delete;

private:
    NearbyReadableRegionCache* previous_;
    bool active_;
};

static bool NearbyIsReadable(const void* pointer, size_t size) {
    NearbyReadableRegionCache* cache = g_nearbyReadableRegionCache;
    if (!cache) return IsReadable(pointer, size);
    ++cache->checks;
    if (!pointer || size == 0) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(pointer);
    if (start < 0x10000 || start > 0x00007FFFFFFFFFFFULL ||
        start + size < start) return false;
    const uintptr_t requestedEnd = start + size;

    if (cache->reuseEnabled) {
        // Sequential registry/inventory walks touch the same committed region
        // for many consecutive checks.  Start at the last hit (round-robin for
        // safety) so the common case is one compare instead of a full linear
        // scan over up to 256 cached regions.
        size_t count = cache->count;
        size_t startIndex = count ? (cache->lastHit % count) : 0;
        for (size_t i = 0; i < count; ++i) {
            const size_t idx = (startIndex + i) % count;
            const NearbyReadableRegion& region = cache->regions[idx];
            if (start >= region.begin && requestedEnd <= region.end) {
                ++cache->cacheHits;
                cache->lastHit = idx;
                return true;
            }
        }
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    ++cache->virtualQueries;
    const LONGLONG queryBegin = NearbyPerformanceCounter();
    const SIZE_T queryResult = VirtualQuery(pointer, &mbi, sizeof(mbi));
    const LONGLONG queryTicks = NearbyPerformanceCounter() - queryBegin;
    cache->virtualQueryTicks += queryTicks;
    if (!cache->reuseEnabled) {
        ++cache->liveVirtualQueries;
        cache->liveVirtualQueryTicks += queryTicks;
    }
    if (queryResult != sizeof(mbi) ||
        mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const uintptr_t regionBegin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t regionEnd = regionBegin + mbi.RegionSize;
    if (regionEnd < regionBegin || start < regionBegin ||
        requestedEnd > regionEnd) return false;

    if (cache->reuseEnabled) {
        size_t index = cache->count;
        if (cache->count < NEARBY_READABLE_REGION_CACHE_SIZE) {
            ++cache->count;
        } else {
            index = cache->replacement;
            cache->replacement =
                (cache->replacement + 1) % NEARBY_READABLE_REGION_CACHE_SIZE;
        }
        cache->regions[index] = {regionBegin, regionEnd};
    }
    return true;
}

// The panel is a no-activate popup so the game remains unpaused.  Suppress the
// game's own keyboard/controller command resolver only while one outer
// CState_Main original update is executing.  This is data-only, main-thread
// scoped, and never patches or detours the hot input-active routine.
struct NearbyCallbackInputMask {
    volatile SHORT* enableWord;
    SHORT savedWord;
    SHORT maskedWord;
    bool changed;
};

static bool NearbyBeginCallbackInputMask(
        void* input, NearbyCallbackInputMask* mask) {
    if (!mask) return false;
    *mask = {};
    if (!ProductionModalInputIsBlocked()) return true;
    if (!PackageWritesAuthorized() || !input ||
        !ProductionForegroundSupportsProductionModal()) return false;
    volatile SHORT* enableWord = reinterpret_cast<volatile SHORT*>(
        reinterpret_cast<unsigned char*>(input) +
        NEARBY_INPUT_ENABLE_WORD_OFFSET);
    if ((reinterpret_cast<uintptr_t>(enableWord) & (alignof(SHORT) - 1)) != 0 ||
        !IsReadable(const_cast<const SHORT*>(enableWord), sizeof(SHORT)))
        return false;

    const SHORT saved = InterlockedCompareExchange16(enableWord, 0, 0);
    const SHORT masked = static_cast<SHORT>(
        static_cast<unsigned short>(saved) & 0xff00u);
    mask->enableWord = enableWord;
    mask->savedWord = saved;
    mask->maskedWord = masked;
    if ((static_cast<unsigned short>(saved) & 0x00ffu) == 0) return true;
    if (InterlockedCompareExchange16(enableWord, masked, saved) != saved) {
        *mask = {};
        return false;
    }
    MemoryBarrier();
    mask->changed = true;
    return true;
}

static bool NearbyRestoreCallbackInputMask(
        void* state, void* input, NearbyCallbackInputMask* mask) {
    if (!mask || !mask->changed) return true;
    if (!state || !input ||
        !IsReadable(reinterpret_cast<unsigned char*>(state) +
                        NEARBY_STATE_INPUT_OFFSET,
                    sizeof(void*)) ||
        *reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(state) +
                                  NEARBY_STATE_INPUT_OFFSET) != input ||
        !IsReadable(const_cast<const SHORT*>(mask->enableWord),
                    sizeof(SHORT))) return false;
    MemoryBarrier();
    SHORT observed = InterlockedCompareExchange16(
        mask->enableWord, mask->savedWord, mask->maskedWord);
    if (observed == mask->maskedWord) {
        mask->changed = false;
        return true;
    }
    // Preserve a game-updated adjacent +0x1AD byte.  If the enable byte is
    // still the zero we published, restore only that byte with a bounded CAS
    // loop; if it is already nonzero, another legitimate path restored it.
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        if ((static_cast<unsigned short>(observed) & 0x00ffu) != 0) {
            mask->changed = false;
            return true;
        }
        const SHORT desired = static_cast<SHORT>(
            (static_cast<unsigned short>(observed) & 0xff00u) |
            (static_cast<unsigned short>(mask->savedWord) & 0x00ffu));
        const SHORT previous = InterlockedCompareExchange16(
            mask->enableWord, desired, observed);
        if (previous == observed) {
            mask->changed = false;
            return true;
        }
        observed = previous;
    }
    return false;
}

static StatusTypeInfo NearbyReadStatusType(void* status) {
    StatusTypeInfo info = {};
    info.kind = GimmickKind::Unresolved;
    strcpy_s(info.moduleName, "unresolved");
    if (!NearbyIsReadable(
            status, GIMMICK_DATA_HOLDER_OFFSET + sizeof(void*))) return info;
    info.holder = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(status) + GIMMICK_DATA_HOLDER_OFFSET);
    if (!NearbyIsReadable(info.holder, sizeof(void*))) return info;
    info.data = *reinterpret_cast<void**>(info.holder);
    if (!NearbyIsReadable(
            info.data, GIMMICK_MODULE_NAME_OFFSET + sizeof(void*))) return info;
    const char* moduleName = *reinterpret_cast<const char**>(
        reinterpret_cast<uintptr_t>(info.data) + GIMMICK_MODULE_NAME_OFFSET);
    if (!NearbyIsReadable(moduleName, sizeof(info.moduleName))) return info;

    size_t length = 0;
    while (length + 1 < sizeof(info.moduleName) &&
           moduleName[length] != '\0') ++length;
    if (moduleName[length] != '\0') return info;
    memcpy(info.moduleName, moduleName, length + 1);
    if (strcmp(info.moduleName, "gimmick_scarecrow") == 0) {
        info.kind = GimmickKind::Scarecrow;
    } else if (strcmp(info.moduleName, "gimmick_sprinkler") == 0) {
        info.kind = GimmickKind::Sprinkler;
    } else {
        info.kind = GimmickKind::Other;
    }
    return info;
}

static bool NearbyReadPointer(const void* object, uintptr_t offset, void** output) {
    if (!object || !output) return false;
    const void* field = reinterpret_cast<const void*>(
        reinterpret_cast<uintptr_t>(object) + offset);
    if (!NearbyIsReadable(field, sizeof(void*))) return false;
    *output = *reinterpret_cast<void* const*>(field);
    return *output != nullptr;
}

static bool NearbyReadItem(void* item, NearbyItemInfo* output) {
    if (!item || !output ||
        !NearbyIsReadable(item, NEARBY_ITEM_RANK_OFFSET + sizeof(int))) return false;
    const LONG references = *reinterpret_cast<volatile LONG*>(
        reinterpret_cast<uintptr_t>(item) + sizeof(void*));
    if (references <= 0) return false;

    // The whole CItemStatus range through rank_ was already validated above,
    // so checking the +0x240 field with a second VirtualQuery is redundant.
    void* holder = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(item) + NEARBY_ITEM_DATA_HOLDER_OFFSET);
    void* data = nullptr;
    if (!holder || !NearbyReadPointer(holder, 0, &data) ||
        !NearbyIsReadable(data, sizeof(u64))) return false;

    output->itemId = *reinterpret_cast<const u64*>(data);
    output->stackCount = *reinterpret_cast<const int*>(
        reinterpret_cast<uintptr_t>(item) + NEARBY_ITEM_STACK_COUNT_OFFSET);
    output->rank = *reinterpret_cast<const int*>(
        reinterpret_cast<uintptr_t>(item) + NEARBY_ITEM_RANK_OFFSET);
    return output->itemId != 0 && output->stackCount > 0 &&
           output->stackCount <= 999;
}

static bool NearbyReadStackCount(void* item, int* output) {
    if (!item || !output ||
        !NearbyIsReadable(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(item) +
                                                   NEARBY_ITEM_STACK_COUNT_OFFSET),
                          sizeof(int))) return false;
    const int count = *reinterpret_cast<const int*>(
        reinterpret_cast<uintptr_t>(item) + NEARBY_ITEM_STACK_COUNT_OFFSET);
    if (count < 0 || count > 999) return false;
    *output = count;
    return true;
}

static bool NearbyReadRawInventory(void* status, NearbyRawPointerVector* output) {
    if (!status || !output ||
        !NearbyIsReadable(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(status) +
                                                  NEARBY_STATUS_ITEMS_OFFSET),
                          sizeof(*output))) return false;
    *output = *reinterpret_cast<const NearbyRawPointerVector*>(
        reinterpret_cast<uintptr_t>(status) + NEARBY_STATUS_ITEMS_OFFSET);
    const uintptr_t begin = reinterpret_cast<uintptr_t>(output->begin);
    const uintptr_t end = reinterpret_cast<uintptr_t>(output->end);
    const uintptr_t capacity = reinterpret_cast<uintptr_t>(output->capacity);
    if (!begin || end < begin || capacity < end ||
        (end - begin) % sizeof(void*) != 0 ||
        (capacity - begin) % sizeof(void*) != 0) return false;
    const size_t count = (end - begin) / sizeof(void*);
    const size_t capacityCount = (capacity - begin) / sizeof(void*);
    return count == NEARBY_CHEST_SLOT_COUNT &&
           capacityCount >= count && capacityCount <= 64 &&
           NearbyIsReadable(output->begin, count * sizeof(void*));
}

// Relaxed version of NearbyReadRawInventory that accepts 1..64 slots.
// Used for non-chest containers (e.g. storage jars) whose slot count
// may differ from the standard 30-slot chest.
static bool NearbyReadRawInventoryRelaxed(void* status,
                                           NearbyRawPointerVector* output) {
    if (!status || !output ||
        !NearbyIsReadable(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(status) +
                                                  NEARBY_STATUS_ITEMS_OFFSET),
                          sizeof(*output))) return false;
    *output = *reinterpret_cast<const NearbyRawPointerVector*>(
        reinterpret_cast<uintptr_t>(status) + NEARBY_STATUS_ITEMS_OFFSET);
    const uintptr_t begin = reinterpret_cast<uintptr_t>(output->begin);
    const uintptr_t end = reinterpret_cast<uintptr_t>(output->end);
    const uintptr_t capacity = reinterpret_cast<uintptr_t>(output->capacity);
    if (!begin || end < begin || capacity < end ||
        (end - begin) % sizeof(void*) != 0 ||
        (capacity - begin) % sizeof(void*) != 0) return false;
    const size_t count = (end - begin) / sizeof(void*);
    const size_t capacityCount = (capacity - begin) / sizeof(void*);
    return count >= 1 && count <= 64 &&
           capacityCount >= count && capacityCount <= 64 &&
           NearbyIsReadable(output->begin, count * sizeof(void*));
}

// v1.5.85【P2 修正】原实现无空指针校验：item==nullptr 时目标地址是 0x8，
//   InterlockedIncrement 直接访问违例 → 进程崩溃。配对的
//   NearbyReleaseOwnedReference 有 `!item` 保护，两者防护强度不对称。
static void NearbyAddReference(void* item) {
    if (!item) return;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(
        reinterpret_cast<uintptr_t>(item) + sizeof(void*)));
}

static void NearbyReleaseOwnedReference(void* item) {
    if (!item || !g_nearbyIntrusiveRelease) return;
    void* owned = item;
    g_nearbyIntrusiveRelease(&owned);
}

static bool NearbyCheckSignature(const char* name, const void* address,
                                 const unsigned char* expected, size_t length) {
    if (!address || !NearbyIsReadable(address, length) ||
        memcmp(address, expected, length) != 0) {
        Log("[NearbySort] native signature FAILED: %s address=%p; disabled safely\n",
            name, address);
        return false;
    }
    return true;
}

static bool NearbyInstallMainHook() {
    static const unsigned char expected[] = SIG_NEARBY_MAIN_UPDATE;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    unsigned char* target = reinterpret_cast<unsigned char*>(
        base + RVA_NEARBY_MAIN_UPDATE);
    if ((reinterpret_cast<uintptr_t>(target) & 7) != 0 ||
        memcmp(target, expected, sizeof(expected)) != 0) {
        Log("[NearbySort] main-state hook signature FAILED; disabled safely\n");
        return false;
    }
    unsigned char* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 96, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        Log("[NearbySort] main-state trampoline allocation failed error=%lu\n",
            GetLastError());
        return false;
    }
    // The first instruction is exactly five bytes.  Replaying only that whole
    // instruction lets the live entry patch preserve every byte from +5 onward.
    memcpy(trampoline, expected, 5);
    unsigned char jumpBack[14] = { 0xff, 0x25, 0, 0, 0, 0 };
    *reinterpret_cast<u64*>(jumpBack + 6) =
        reinterpret_cast<u64>(target + 5);
    memcpy(trampoline + 5, jumpBack, sizeof(jumpBack));
    DWORD trampolineProtection = 0;
    if (!VirtualProtect(trampoline, 96, PAGE_EXECUTE_READ,
                        &trampolineProtection) ||
        !FlushInstructionCache(GetCurrentProcess(), trampoline, 96)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        Log("[NearbySort] main-state trampoline finalize FAILED error=%lu; "
            "disabled safely\n", GetLastError());
        return false;
    }
    g_nearbyMainTrampoline = trampoline;
    g_nearbyOriginalMainUpdate.store(
        reinterpret_cast<NearbyMainUpdateFunction>(trampoline),
        std::memory_order_release);
    return true;
}

static bool __fastcall NearbyMainUpdateDetour(void* state, void* updateInfo) {
    // v1.1.31: 帧率自适应节流——帧间隔突降时跳过辅助逻辑只调原生函数
    // 跑到新区域时游戏帧率突然下降，此时 detour 的每帧开销会叠加卡顿
    // 降级模式下只调原生函数，跳过 ProductionAuto/AutoPet 辅助逻辑
    // 用 EMA 基线 + 突降阈值，适配不同帧率（60fps/165fps 均可）
    {
        const ULONGLONG now = GetTickCount64();
        if (g_nearbyLastFrameTick != 0) {
            const double frameMs = static_cast<double>(now - g_nearbyLastFrameTick);
            if (g_nearbyThrottled) {
                // 降级模式：只看是否恢复（不更新基线，避免被卡顿拉高）
                if (frameMs < g_nearbyFrameBaselineMs + NEARBY_THROTTLE_RECOVERY_MS) {
                    g_nearbyThrottled = false;
                }
            } else {
                // 正常模式：更新 EMA 基线
                if (g_nearbyFrameBaselineMs == 0.0) {
                    g_nearbyFrameBaselineMs = frameMs;  // 首帧
                } else {
                    g_nearbyFrameBaselineMs = g_nearbyFrameBaselineMs * 0.9 + frameMs * 0.1;
                }
                // 检测突降
                if (frameMs > g_nearbyFrameBaselineMs + NEARBY_THROTTLE_SPIKE_MS) {
                    g_nearbyThrottled = true;
                }
            }
        }
        g_nearbyLastFrameTick = now;

        // 降级模式：快速路径，只调原生函数
        if (g_nearbyThrottled) {
            NearbyMainUpdateFunction original = g_nearbyOriginalMainUpdate.load(
                std::memory_order_acquire);
            const bool result = original ? original(state, updateInfo) : false;
            g_nearbyLastMainUpdateAt = now;
            return result;
        }
    }

    const u64 productionCallbackSequence = ProductionBeginMainCallback();
    // A native output helper or status notification may synchronously re-enter
    // this dispatcher.  Nothing except the original game update is eligible in
    // that nested call: in particular, do not begin AutoPet, publish lifecycle
    // or input edges, reset Production breadcrumbs, or run another Mod native
    // transaction under the outer transaction.
    if (productionCallbackSequence == 0) {
        NearbyMainUpdateFunction nestedOriginal =
            g_nearbyOriginalMainUpdate.load(std::memory_order_acquire);
        void* nestedInput = nullptr;
        if (state && IsReadable(
                reinterpret_cast<unsigned char*>(state) +
                    NEARBY_STATE_INPUT_OFFSET,
                sizeof(void*))) {
            nestedInput = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(state) +
                NEARBY_STATE_INPUT_OFFSET);
        }
        NearbyCallbackInputMask nestedInputMask = {};
        const bool nestedInputMaskReady = NearbyBeginCallbackInputMask(
            nestedInput, &nestedInputMask);
        const bool nestedResult =
            nestedOriginal ? nestedOriginal(state, updateInfo) : false;
        const bool nestedInputRestored = NearbyRestoreCallbackInputMask(
            state, nestedInput, &nestedInputMask);
        // Keep the DirectInput/IAT modal gate published for the whole native
        // call even when the callback-local data mask could not be installed.
        // Releasing before nestedOriginal would expose the same cached input
        // edge to the game.  Teardown is therefore strictly post-original.
        if (!nestedInputMaskReady || !nestedInputRestored)
            ProductionForceReleaseModalInputBlock();
        ProductionCompleteMainCallback(0);
        return nestedResult;
    }
    const u64 autoPetCallbackSequence = AutoPetBeginMainCallback();
    // Capture the AutoPet latch at callback entry.  A reload completion that
    // happens inside this native callback is deliberately ineligible until
    // the following callback, so "post cleanup" can never collapse into the
    // same frame that published readiness.
    const u64 autoPetReadyGenerationAtEntry =
        AutoPetCaptureMorningReadyGeneration();
    const u64 autoPetPendingDailyGenerationAtEntry =
        AutoPetCapturePendingDailyGeneration();
    const u64 autoPetReloadInvocationGenerationAtEntry =
        AutoPetCaptureReloadInvocationGeneration();
    const u64 autoPetLoadReadyGenerationAtEntry =
        AutoPetCaptureLoadReadyGeneration();
    const u64 autoPetLoadInFlightGenerationAtEntry =
        AutoPetCaptureLoadInFlightGeneration();
    const ULONGLONG now = GetTickCount64();
    const bool callbackWasInterrupted =
        g_nearbyLastMainUpdateAt == 0 ||
        now - g_nearbyLastMainUpdateAt > NEARBY_MAIN_CALLBACK_GAP_MS;

    bool hotkeyContextValid = false;
    bool productionContextObserved = false;
    void* productionInput = nullptr;
    if (g_nearbySortReady.load(std::memory_order_acquire) && state &&
        NearbyIsReadable(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(state) +
                                                 NEARBY_STATE_INPUT_OFFSET),
                         sizeof(void*))) {
        void* input = *reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(state) + NEARBY_STATE_INPUT_OFFSET);
        if (input) {
            productionInput = input;
            const bool sameInputIdentity =
                g_nearbyLastInput != nullptr && input == g_nearbyLastInput;
            const bool productionFeatureEnabled =
                ProductionFeatureIsEnabled();
            // Scheduler lifecycle is the verified CState_Main/input identity,
            // not the transient native hotkey-eligibility predicate.  A real
            // input-manager replacement publishes a world edge; an ordinary
            // callback gap or UI/input suppression does not.
            if (g_nearbyLastInput != nullptr && !sameInputIdentity)
                ProductionObserveWorldContext(
                    false, autoPetReadyGenerationAtEntry);
            const bool productionWorldContextValid =
                productionFeatureEnabled;
            ProductionObserveWorldContext(
                productionWorldContextValid,
                sameInputIdentity ? 0 : autoPetReadyGenerationAtEntry);
            productionContextObserved = true;
            if (callbackWasInterrupted || input != g_nearbyLastInput) {
                NearbyDisarmCustomHotkey();
            }
            g_nearbyLastInput = input;
            hotkeyContextValid = true;
            // Nearby chest sort has been removed from this MOD.  The
            // infrastructure (readable cache, spatial search, inventory
            // readers) remains as the foundation for production automation.
            NearbyDisarmCustomHotkey();
        }
    }
    if (!hotkeyContextValid) {
        g_nearbyLastInput = nullptr;
        NearbyDisarmCustomHotkey();
    }
    if (!productionContextObserved) {
        ProductionObserveWorldContext(
            false, autoPetReadyGenerationAtEntry);
    }
    // Production commits finished outputs before the native update.  The
    // engine therefore observes the new machine/chest topology in this same
    // callback, matching the phase in which its Lua/native interaction path
    // normally mutates those objects.  A hotkey accepted above has already
    // armed the scan/cooldown gate, so it can never fall through to a transfer
    // in the key callback.
    ProductionSetMainCallbackPhase(2);
    ProductionOnMainThreadPreUpdate(productionCallbackSequence,
                                    callbackWasInterrupted);
    ProductionSetMainCallbackPhase(3);
    NearbyMainUpdateFunction original = g_nearbyOriginalMainUpdate.load(
        std::memory_order_acquire);
    NearbyCallbackInputMask callbackInputMask = {};
    const bool callbackInputMaskReady = NearbyBeginCallbackInputMask(
        productionInput, &callbackInputMask);
    if (!callbackInputMaskReady) {
        if (!g_nearbyModalInputMaskFaultLogged.exchange(
                true, std::memory_order_acq_rel)) {
            Log("[NearbySort] modal callback-local input mask FAILED before "
                "original update; modal gate retained through original and "
                "release deferred\n");
        }
    }
    const bool result = original ? original(state, updateInfo) : false;
    const bool callbackInputRestored = NearbyRestoreCallbackInputMask(
        state, productionInput, &callbackInputMask);
    if (!callbackInputRestored) {
        if (!g_nearbyModalInputMaskFaultLogged.exchange(
                true, std::memory_order_acq_rel)) {
            Log("[NearbySort] modal callback-local input restore FAILED after "
                "original update; modal released safely\n");
        }
    }
    // Both begin-mask and restore failures are fail-closed across the native
    // original update.  Only after it returns may the packed modal generation
    // be force-released and the worker hide the panel/capture.
    if (!callbackInputMaskReady || !callbackInputRestored) {
        ProductionForceReleaseModalInputBlock();
    }
    // The verified native CState_Main body must finish before AutoPet consumes
    // the entry snapshot.  This gives live livestock/home state one complete
    // native update after reload cleanup, while still keeping the whole
    // canonical morning batch inside this single callback.
    AutoPetOnMainThreadUpdate(
        state, autoPetReadyGenerationAtEntry,
        autoPetPendingDailyGenerationAtEntry,
        autoPetReloadInvocationGenerationAtEntry,
        autoPetLoadReadyGenerationAtEntry,
        autoPetLoadInFlightGenerationAtEntry,
        autoPetCallbackSequence);
    AutoPetCompleteMainCallback(autoPetCallbackSequence);
    // This consumer is deliberately last.  AutoPet's established morning and
    // load-once schedule, filters and generation latches are complete before
    // Production observes the canonical serialized registry.
    ProductionSetMainCallbackPhase(4);
    ProductionOnMainThreadUpdate();
    // Store callback completion, not entry.  Slow work inside this frame must
    // not make the following frame look like a lifecycle interruption.
    g_nearbyLastMainUpdateAt = GetTickCount64();
    ProductionCompleteMainCallback(productionCallbackSequence);
    return result;
}

static bool NearbyFinishMainHook() {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    unsigned char* target = reinterpret_cast<unsigned char*>(
        base + RVA_NEARBY_MAIN_UPDATE);
    unsigned char* relay = static_cast<unsigned char*>(
        AllocateExecutableNear(reinterpret_cast<uintptr_t>(target), 32));
    if (!relay) {
        g_nearbyOriginalMainUpdate.store(nullptr, std::memory_order_release);
        if (g_nearbyMainTrampoline) {
            VirtualFree(g_nearbyMainTrampoline, 0, MEM_RELEASE);
            g_nearbyMainTrampoline = nullptr;
        }
        Log("[NearbySort] main-state near relay allocation failed error=%lu; "
            "disabled safely\n", GetLastError());
        return false;
    }

    unsigned char relayCode[14] = { 0xff, 0x25, 0, 0, 0, 0 };
    *reinterpret_cast<u64*>(relayCode + 6) =
        reinterpret_cast<u64>(&NearbyMainUpdateDetour);
    memcpy(relay, relayCode, sizeof(relayCode));
    DWORD relayProtection = 0;
    if (!VirtualProtect(relay, 32, PAGE_EXECUTE_READ, &relayProtection) ||
        !FlushInstructionCache(GetCurrentProcess(), relay, sizeof(relayCode))) {
        VirtualFree(relay, 0, MEM_RELEASE);
        g_nearbyOriginalMainUpdate.store(nullptr, std::memory_order_release);
        VirtualFree(g_nearbyMainTrampoline, 0, MEM_RELEASE);
        g_nearbyMainTrampoline = nullptr;
        Log("[NearbySort] main-state relay finalize FAILED error=%lu; "
            "disabled safely\n", GetLastError());
        return false;
    }

    const std::int64_t displacement =
        reinterpret_cast<std::int64_t>(relay) -
        static_cast<std::int64_t>(reinterpret_cast<uintptr_t>(target) + 5);
    if (displacement < INT32_MIN || displacement > INT32_MAX) {
        VirtualFree(relay, 0, MEM_RELEASE);
        g_nearbyOriginalMainUpdate.store(nullptr, std::memory_order_release);
        VirtualFree(g_nearbyMainTrampoline, 0, MEM_RELEASE);
        g_nearbyMainTrampoline = nullptr;
        Log("[NearbySort] main-state near relay displacement invalid; disabled safely\n");
        return false;
    }

    unsigned char expected[] = SIG_NEARBY_MAIN_UPDATE;
    unsigned char hook[8] = {};
    memcpy(hook, expected, sizeof(hook));
    hook[0] = 0xe9;
    *reinterpret_cast<std::int32_t*>(hook + 1) =
        static_cast<std::int32_t>(displacement);
    LONG64 expectedWord = 0;
    LONG64 hookWord = 0;
    memcpy(&expectedWord, expected, sizeof(expectedWord));
    memcpy(&hookWord, hook, sizeof(hookWord));

    DWORD oldProtection = 0;
    if (!VirtualProtect(target, sizeof(hook), PAGE_EXECUTE_READWRITE,
                        &oldProtection)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        g_nearbyOriginalMainUpdate.store(nullptr, std::memory_order_release);
        VirtualFree(g_nearbyMainTrampoline, 0, MEM_RELEASE);
        g_nearbyMainTrampoline = nullptr;
        Log("[NearbySort] main-state hook protection change FAILED error=%lu; "
            "disabled safely\n", GetLastError());
        return false;
    }
    const LONG64 observed = InterlockedCompareExchange64(
        reinterpret_cast<volatile LONG64*>(target), hookWord, expectedWord);
    DWORD ignored = 0;
    const bool protectionRestored =
        VirtualProtect(target, sizeof(hook), oldProtection, &ignored) != FALSE;
    const bool entryFlushed =
        FlushInstructionCache(GetCurrentProcess(), target, sizeof(hook)) != FALSE;
    if (observed != expectedWord || memcmp(target, hook, sizeof(hook)) != 0 ||
        !protectionRestored || !entryFlushed) {
        // If the compare failed, the relay is unreachable and can be freed.  If
        // readback alone failed, retain all code because another thread may
        // already have entered the published jump.
        if (observed != expectedWord) {
            VirtualFree(relay, 0, MEM_RELEASE);
            g_nearbyOriginalMainUpdate.store(nullptr, std::memory_order_release);
            VirtualFree(g_nearbyMainTrampoline, 0, MEM_RELEASE);
            g_nearbyMainTrampoline = nullptr;
        }
        Log("[NearbySort] main-state atomic hook finalize FAILED: compare=%s "
            "readback=%s protection=%s flush=%s; disabled safely\n",
            observed == expectedWord ? "OK" : "FAILED",
            memcmp(target, hook, sizeof(hook)) == 0 ? "OK" : "FAILED",
            protectionRestored ? "OK" : "FAILED",
            entryFlushed ? "OK" : "FAILED");
        return false;
    }
    Log("[NearbySort] main-state hook published with aligned atomic 8-byte entry\n");
    g_nearbyMainRelay = relay;  // v1.1.49: save for unload (H2)
    return true;
}

static bool InstallNearbyChestSort() {
    if (!PackageWritesAuthorized()) {
        Log("[NearbySort] package verification gate is closed\n");
        return false;
    }
    g_nearbySortFaulted.store(false, std::memory_order_relaxed);
    g_nearbyQuarantinedItem.store(nullptr, std::memory_order_relaxed);
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    static const unsigned char inputActiveExpected[] = SIG_NEARBY_INPUT_ACTIVE;
    static const unsigned char inputConsumeExpected[] = SIG_NEARBY_INPUT_CONSUME;
    static const unsigned char inputEventExpected[] = SIG_NEARBY_INPUT_EVENT;
    static const unsigned char spatialExpected[] = SIG_NEARBY_SPATIAL_SEARCH;
    static const unsigned char resolveExpected[] = SIG_NEARBY_RESOLVE_WORLD_OBJECT;
    static const unsigned char freeExpected[] = SIG_NEARBY_RAW_VECTOR_FREE;
    static const unsigned char refreshExpected[] = SIG_NEARBY_REFRESH_WORLD_OBJECT;
    // Entire function through RET.  Besides the CommandItem* ABI, this pins
    // the exact native policy: null item => 999; stackable item (direct call
    // to 0xffdf0) => 999-count at item+0x260; otherwise zero.
    static const unsigned char capacityExpected[] = SIG_NEARBY_COMMAND_CAPACITY;
    static const unsigned char releaseExpected[] = SIG_NEARBY_INTRUSIVE_RELEASE;
    static const unsigned char itemAdjustExpected[] = SIG_NEARBY_ITEM_ADJUST;
    static const unsigned char forbiddenExpected[] = SIG_NEARBY_FORBIDDEN_ITEM;
    static const unsigned char playerCapacityExpected[] = SIG_NEARBY_PLAYER_CAPACITY;
    static const unsigned char batchRecalculateExpected[] = SIG_NEARBY_BATCH_RECALCULATE;
    static const unsigned char batchUiRefreshExpected[] = SIG_NEARBY_BATCH_UI_REFRESH;
    static const unsigned char batchDirtyExpected[] = SIG_NEARBY_BATCH_DIRTY;
    void* spatialEntry = g_originalSpatialTest
        ? reinterpret_cast<void*>(g_originalSpatialTest)
        : reinterpret_cast<void*>(base + RVA_PLACEMENT_SPATIAL_TEST);
    if (!NearbyCheckSignature("input active gate", reinterpret_cast<void*>(
                                  base + RVA_NEARBY_INPUT_ACTIVE),
                              inputActiveExpected, sizeof(inputActiveExpected)) ||
        !NearbyCheckSignature("input edge consumer", reinterpret_cast<void*>(
                                  base + RVA_NEARBY_INPUT_CONSUME),
                              inputConsumeExpected, sizeof(inputConsumeExpected)) ||
        !NearbyCheckSignature("input command event", reinterpret_cast<void*>(
                                  base + RVA_NEARBY_INPUT_EVENT),
                              inputEventExpected, sizeof(inputEventExpected)) ||
        !NearbyCheckSignature("spatial search", spatialEntry,
                              spatialExpected, sizeof(spatialExpected)) ||
        !NearbyCheckSignature("world object resolver", reinterpret_cast<void*>(
                                  base + RVA_NEARBY_RESOLVE_WORLD_OBJECT),
                              resolveExpected, sizeof(resolveExpected)) ||
        !NearbyCheckSignature("world object refresh", reinterpret_cast<void*>(
                                  base + RVA_NEARBY_REFRESH_WORLD_OBJECT),
                              refreshExpected, sizeof(refreshExpected)) ||
        !NearbyCheckSignature("raw vector free", reinterpret_cast<void*>(
                                  base + RVA_NEARBY_RAW_VECTOR_FREE),
                              freeExpected, sizeof(freeExpected)) ||
        !NearbyCheckSignature("CommandItem capacity", reinterpret_cast<void*>(
                                  base + RVA_NEARBY_COMMAND_CAPACITY),
                              capacityExpected, sizeof(capacityExpected)) ||
        !NearbyCheckSignature("intrusive release", reinterpret_cast<void*>(
                                  base + RVA_NEARBY_INTRUSIVE_RELEASE),
                              releaseExpected, sizeof(releaseExpected)) ||
        !NearbyCheckSignature("item count adjust", reinterpret_cast<void*>(
                                  base + RVA_NEARBY_ITEM_ADJUST),
                              itemAdjustExpected, sizeof(itemAdjustExpected)) ||
        !NearbyCheckSignature("forbidden item predicate", reinterpret_cast<void*>(
                                  base + RVA_NEARBY_FORBIDDEN_ITEM),
                              forbiddenExpected, sizeof(forbiddenExpected)) ||
        !NearbyCheckSignature("player inventory capacity", reinterpret_cast<void*>(
                                  base + RVA_NEARBY_PLAYER_CAPACITY),
                              playerCapacityExpected,
                              sizeof(playerCapacityExpected)) ||
        !NearbyCheckSignature("batch inventory recalculate", reinterpret_cast<void*>(
                                  base + RVA_NEARBY_BATCH_RECALCULATE),
                              batchRecalculateExpected,
                              sizeof(batchRecalculateExpected)) ||
        !NearbyCheckSignature("batch UI refresh", reinterpret_cast<void*>(
                                  base + RVA_NEARBY_BATCH_UI_REFRESH),
                              batchUiRefreshExpected,
                              sizeof(batchUiRefreshExpected)) ||
        !NearbyCheckSignature("batch dirty notification", reinterpret_cast<void*>(
                                  base + RVA_NEARBY_BATCH_DIRTY),
                              batchDirtyExpected,
                              sizeof(batchDirtyExpected))) return false;

    void** callbackVtable = reinterpret_cast<void**>(
        base + RVA_NEARBY_SEARCH_CALLBACK_VTABLE);
    if (!NearbyIsReadable(callbackVtable, sizeof(void*) * 5) ||
        callbackVtable[0] != reinterpret_cast<void*>(
            base + RVA_NEARBY_SEARCH_CALLBACK_COPY) ||
        callbackVtable[2] != reinterpret_cast<void*>(
            base + RVA_NEARBY_SEARCH_CALLBACK_INVOKE) ||
        callbackVtable[4] != reinterpret_cast<void*>(
            base + RVA_NEARBY_SEARCH_CALLBACK_DESTROY)) {
        Log("[NearbySort] native spatial callback vtable FAILED; disabled safely\n");
        return false;
    }

    g_nearbyInputActive = reinterpret_cast<NearbyInputActiveFunction>(
        base + RVA_NEARBY_INPUT_ACTIVE);
    g_nearbyInputConsume = reinterpret_cast<NearbyInputConsumeFunction>(
        base + RVA_NEARBY_INPUT_CONSUME);
    g_nearbyInputEvent = reinterpret_cast<NearbyInputEventFunction>(
        base + RVA_NEARBY_INPUT_EVENT);
    g_nearbySpatialSearch = reinterpret_cast<NearbySpatialSearchFunction>(spatialEntry);
    g_nearbyResolveWorldObject = reinterpret_cast<NearbyResolveWorldObjectFunction>(
        base + RVA_NEARBY_RESOLVE_WORLD_OBJECT);
    g_nearbyRefreshWorldObject = reinterpret_cast<NearbyRefreshWorldObjectFunction>(
        base + RVA_NEARBY_REFRESH_WORLD_OBJECT);
    g_nearbyRawVectorFree = reinterpret_cast<NearbyRawVectorFreeFunction>(
        base + RVA_NEARBY_RAW_VECTOR_FREE);
    g_nearbyCommandCapacity = reinterpret_cast<NearbyCommandCapacityFunction>(
        base + RVA_NEARBY_COMMAND_CAPACITY);
    g_nearbyIntrusiveRelease = reinterpret_cast<NearbyIntrusiveReleaseFunction>(
        base + RVA_NEARBY_INTRUSIVE_RELEASE);
    g_nearbyItemAdjust = reinterpret_cast<NearbyItemAdjustFunction>(
        base + RVA_NEARBY_ITEM_ADJUST);
    g_nearbyForbiddenItem = reinterpret_cast<NearbyForbiddenItemFunction>(
        base + RVA_NEARBY_FORBIDDEN_ITEM);
    g_nearbyPlayerCapacity = reinterpret_cast<NearbyPlayerCapacityFunction>(
        base + RVA_NEARBY_PLAYER_CAPACITY);
    g_nearbyBatchRecalculate = reinterpret_cast<NearbyBatchRecalculateFunction>(
        base + RVA_NEARBY_BATCH_RECALCULATE);
    g_nearbyBatchUiRefresh = reinterpret_cast<NearbyBatchUiRefreshFunction>(
        base + RVA_NEARBY_BATCH_UI_REFRESH);
    g_nearbyBatchDirty = reinterpret_cast<NearbyBatchDirtyFunction>(
        base + RVA_NEARBY_BATCH_DIRTY);
    LARGE_INTEGER performanceFrequency = {};
    if (!QueryPerformanceFrequency(&performanceFrequency) ||
        performanceFrequency.QuadPart <= 0) {
        Log("[NearbySort] high-resolution timer unavailable; disabled safely\n");
        return false;
    }
    g_nearbyPerformanceFrequency = performanceFrequency.QuadPart;

    if (!NearbyInstallMainHook() || !NearbyFinishMainHook()) return false;
    g_nearbySortReady.store(true, std::memory_order_release);
    Log("[NearbySort] ready: controller=DPAD_UP native_command=0x3f8 "
        "consume_original_edge=1 keyboard=none "
        "native_input_code_hook=0 modal_foreground_window=0 "
        "callback_local_input_mask=1 no_activate_panel=1 "
        "callback_gap_ms=%llu "
        "radius=%.1f "
        "matching_key=(item_id,rank) existing-stacks-only main-thread=1 "
        "largest_existing_stack_first=1 batch_refresh=1 timing=QPC\n",
        static_cast<unsigned long long>(NEARBY_MAIN_CALLBACK_GAP_MS),
        static_cast<double>(NEARBY_SORT_RADIUS));
    return true;
}

// ============================================================
// v1.1.45-diag: item_adjust 调用链诊断 hook（E9 rel32 安全方案，KB-061 修复版）
// v1.1.44 事故根源：入口 5 字节内写 ff 25 指令头（14 字节指令只写 5 字节，
// 未写入的地址位落在原函数字节上 -> 跳垃圾地址崩溃，任何物品数量变化必崩）。
// 本版结构（与 GameClock / NearbyMain / AutoFish hook 同款成熟模板）：
//   入口(5B):    E9 rel32 -> relay（恰好 5 字节，完整覆盖首指令 48 89 5C 24 08）
//   relay(14B):  ff 25 00 00 00 00 + detour 绝对地址（自有缓冲，完整写入安全）
//   trampoline: 原首指令 5B + ff 25 跳回 target+5
// 原子性: target 8 字节对齐(0x108EB0)，InterlockedCompareExchange64 一次
//   替换完整 8 字节（E9+rel32+原字节后 3B），比较失败则判 FAIL 不写入。
// 开关: HotConfig productionauto 组 DIAG_ITEM_ADJUST（HOT_BOOL，默认 0）。
//   hook 常装（detour 关闭态开销 = 一次原子读+判断），日志按热开关打印。
//   JSON 写 1 后由 mod_tick Poll 热生效，无需重启。
// 日志: [NearbySort][diag-itemadjust] caller=0xRVA item=... delta=...
//       同 caller 400ms 内聚合为 "连续 xN (repeat聚合)"
// 用途: 定位游戏原生加工机投入/取出函数（12 caller 清单对照）+
//       观察官方 v1.20 "长按连续投入/取出" repeat 调用模式
// ============================================================
#include <intrin.h>  // _ReturnAddress

static volatile long g_diagItemAdjustEnabled = 0;
static void* g_diagItemAdjustTramp = nullptr;
static void* g_diagItemAdjustRelay = nullptr;
static unsigned char g_diagItemAdjustSaved[8] = { 0 };
static bool g_diagItemAdjustInstalled = false;

typedef void (__fastcall* NearbyItemAdjustDiagFunction)(void*, int);

static void NearbyDiagItemAdjustLog(uintptr_t callerRva, void* item, int delta) {
    static uintptr_t s_lastRva = 0;
    static ULONGLONG s_lastTick = 0;
    static int s_burst = 0;
    const ULONGLONG now = GetTickCount64();
    if (callerRva == s_lastRva && now - s_lastTick < 400) {
        ++s_burst;
        s_lastTick = now;
        return;
    }
    if (s_burst > 0) {
        Log("[NearbySort][diag-itemadjust] caller=0x%llX 连续 x%d (repeat聚合)\n",
            static_cast<unsigned long long>(s_lastRva), s_burst + 1);
        s_burst = 0;
    }
    Log("[NearbySort][diag-itemadjust] caller=0x%llX item=%p delta=%d\n",
        static_cast<unsigned long long>(callerRva), item, delta);
    s_lastRva = callerRva;
    s_lastTick = now;
}

static void __fastcall NearbyItemAdjustDiagDetour(void* item, int delta) {
    if (g_diagItemAdjustEnabled) {
        static uintptr_t s_cachedBase = 0;
        if (!s_cachedBase) {
            s_cachedBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        }
        NearbyDiagItemAdjustLog(
            reinterpret_cast<uintptr_t>(_ReturnAddress()) - s_cachedBase,
            item, delta);
    }
    if (g_diagItemAdjustTramp) {
        reinterpret_cast<NearbyItemAdjustDiagFunction>(
            g_diagItemAdjustTramp)(item, delta);
    }
}

static bool NearbyInstallItemAdjustDiag() {
    static const unsigned char expected[] = SIG_NEARBY_ITEM_ADJUST;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) return false;
    unsigned char* target = reinterpret_cast<unsigned char*>(
        base + RVA_NEARBY_ITEM_ADJUST);
    if ((reinterpret_cast<uintptr_t>(target) & 7) != 0) {
        Log("[NearbySort][diag-itemadjust] target 未 8 字节对齐; 跳过安装\n");
        return false;
    }
    if (memcmp(target, expected, sizeof(expected)) != 0) {
        Log("[NearbySort][diag-itemadjust] item_adjust 签名校验 FAIL @0x%llX; 跳过安装\n",
            static_cast<unsigned long long>(RVA_NEARBY_ITEM_ADJUST));
        return false;
    }
    // 1) trampoline: 原首指令(5B 完整指令 48 89 5C 24 08) + ff 25 跳回 target+5
    unsigned char* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 96, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        Log("[NearbySort][diag-itemadjust] trampoline 分配失败 error=%lu\n",
            GetLastError());
        return false;
    }
    memcpy(trampoline, expected, 5);
    unsigned char jumpBack[14] = { 0xff, 0x25, 0, 0, 0, 0 };
    *reinterpret_cast<unsigned long long*>(jumpBack + 6) =
        reinterpret_cast<unsigned long long>(target + 5);
    memcpy(trampoline + 5, jumpBack, sizeof(jumpBack));
    if (!SealExecutableMemory(trampoline, 96)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        Log("[NearbySort][diag-itemadjust] trampoline seal 失败\n");
        return false;
    }
    // 2) relay 近端中继(±2GB 内): 完整 ff 25 十四字节跳 detour（自有缓冲安全）
    unsigned char* relay = static_cast<unsigned char*>(AllocateExecutableNear(
        reinterpret_cast<uintptr_t>(target), 32));
    if (!relay) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        Log("[NearbySort][diag-itemadjust] relay 分配失败 error=%lu\n",
            GetLastError());
        return false;
    }
    unsigned char relayCode[14] = { 0xff, 0x25, 0, 0, 0, 0 };
    *reinterpret_cast<unsigned long long*>(relayCode + 6) =
        reinterpret_cast<unsigned long long>(&NearbyItemAdjustDiagDetour);
    memcpy(relay, relayCode, sizeof(relayCode));
    if (!SealExecutableMemory(relay, 32)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        VirtualFree(trampoline, 0, MEM_RELEASE);
        Log("[NearbySort][diag-itemadjust] relay seal 失败\n");
        return false;
    }
    // 3) 入口 E9 rel32（5 字节恰好），CAS 8 字节原子替换
    const std::int64_t displacement =
        reinterpret_cast<std::int64_t>(relay) -
        static_cast<std::int64_t>(reinterpret_cast<uintptr_t>(target) + 5);
    if (displacement < INT32_MIN || displacement > INT32_MAX) {
        VirtualFree(relay, 0, MEM_RELEASE);
        VirtualFree(trampoline, 0, MEM_RELEASE);
        Log("[NearbySort][diag-itemadjust] relay 超出 rel32 范围; 跳过安装\n");
        return false;
    }
    unsigned char expected8[8];
    unsigned char hook8[8];
    memcpy(expected8, expected, sizeof(expected8));
    memcpy(hook8, expected8, sizeof(hook8));
    hook8[0] = 0xe9;
    *reinterpret_cast<std::int32_t*>(hook8 + 1) =
        static_cast<std::int32_t>(displacement);
    LONG64 expectedWord = 0;
    LONG64 hookWord = 0;
    memcpy(&expectedWord, expected8, sizeof(expectedWord));
    memcpy(&hookWord, hook8, sizeof(hookWord));
    memcpy(g_diagItemAdjustSaved, expected8, sizeof(g_diagItemAdjustSaved));

    DWORD oldProtection = 0;
    if (!VirtualProtect(target, sizeof(hook8), PAGE_EXECUTE_READWRITE,
                        &oldProtection)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        VirtualFree(trampoline, 0, MEM_RELEASE);
        Log("[NearbySort][diag-itemadjust] target 保护切换失败\n");
        return false;
    }
    const LONG64 observed = InterlockedCompareExchange64(
        reinterpret_cast<volatile LONG64*>(target), hookWord, expectedWord);
    DWORD ignored = 0;
    const bool protectionRestored =
        VirtualProtect(target, sizeof(hook8), oldProtection, &ignored) != FALSE;
    const bool entryFlushed =
        FlushInstructionCache(GetCurrentProcess(), target, sizeof(hook8)) != FALSE;
    if (observed != expectedWord || memcmp(target, hook8, sizeof(hook8)) != 0 ||
        !protectionRestored || !entryFlushed) {
        if (observed != expectedWord) {
            VirtualFree(relay, 0, MEM_RELEASE);
            VirtualFree(trampoline, 0, MEM_RELEASE);
        }
        Log("[NearbySort][diag-itemadjust] 原子入口写入 FAILED compare=%s "
            "readback=%s prot=%s flush=%s; 安全跳过\n",
            observed == expectedWord ? "OK" : "FAIL",
            memcmp(target, hook8, sizeof(hook8)) == 0 ? "OK" : "FAIL",
            protectionRestored ? "OK" : "FAIL", entryFlushed ? "OK" : "FAIL");
        return false;
    }
    g_diagItemAdjustTramp = trampoline;
    g_diagItemAdjustRelay = relay;
    g_diagItemAdjustInstalled = true;
    Log("[NearbySort][diag-itemadjust] hook OK @0x%llX E9->relay=%p tramp=%p "
        "(KB-061 安全方案, DIAG_ITEM_ADJUST 热开关控制日志)\n",
        static_cast<unsigned long long>(RVA_NEARBY_ITEM_ADJUST), relay, trampoline);
    return true;
}

static void NearbyRemoveItemAdjustDiag() {
    if (!g_diagItemAdjustInstalled) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (base) {
        unsigned char* target = reinterpret_cast<unsigned char*>(
            base + RVA_NEARBY_ITEM_ADJUST);
        LONG64 savedWord = 0;
        memcpy(&savedWord, g_diagItemAdjustSaved, sizeof(savedWord));
        LONG64 currentWord = 0;
        memcpy(&currentWord, target, sizeof(currentWord));
        DWORD protection = 0;
        if (VirtualProtect(target, 8, PAGE_EXECUTE_READWRITE, &protection)) {
            InterlockedCompareExchange64(
                reinterpret_cast<volatile LONG64*>(target), savedWord, currentWord);
            FlushInstructionCache(GetCurrentProcess(), target, 8);
            VirtualProtect(target, 8, protection, &protection);
        }
    }
    if (g_diagItemAdjustRelay) {
        VirtualFree(g_diagItemAdjustRelay, 0, MEM_RELEASE);
        g_diagItemAdjustRelay = nullptr;
    }
    if (g_diagItemAdjustTramp) {
        VirtualFree(g_diagItemAdjustTramp, 0, MEM_RELEASE);
        g_diagItemAdjustTramp = nullptr;
    }
    g_diagItemAdjustInstalled = false;
    Log("[NearbySort][diag-itemadjust] hook removed\n");
}

// v1.1.49: NearbyMainUpdate hook 还原（H2 修复）
// 参照 NearbyRemoveItemAdjustDiag 模式：CAS 还原原始 8 字节 → 释放 relay + trampoline。
// 原始字节 = SIG_NEARBY_MAIN_UPDATE 前 8 字节（hook 安装时用 CAS 替换的正是这 8 字节）。
static void NearbyRemoveMainHook() {
    if (!g_nearbyMainTrampoline) return;  // hook 未安装
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (base) {
        unsigned char* target = reinterpret_cast<unsigned char*>(
            base + RVA_NEARBY_MAIN_UPDATE);
        unsigned char expected[] = SIG_NEARBY_MAIN_UPDATE;
        LONG64 expectedWord = 0;
        memcpy(&expectedWord, expected, sizeof(expectedWord));
        LONG64 currentWord = 0;
        memcpy(&currentWord, target, sizeof(currentWord));
        DWORD protection = 0;
        if (VirtualProtect(target, 8, PAGE_EXECUTE_READWRITE, &protection)) {
            InterlockedCompareExchange64(
                reinterpret_cast<volatile LONG64*>(target),
                expectedWord, currentWord);
            FlushInstructionCache(GetCurrentProcess(), target, 8);
            VirtualProtect(target, 8, protection, &protection);
        }
    }
    g_nearbyOriginalMainUpdate.store(nullptr, std::memory_order_release);
    if (g_nearbyMainRelay) {
        VirtualFree(g_nearbyMainRelay, 0, MEM_RELEASE);
        g_nearbyMainRelay = nullptr;
    }
    if (g_nearbyMainTrampoline) {
        VirtualFree(g_nearbyMainTrampoline, 0, MEM_RELEASE);
        g_nearbyMainTrampoline = nullptr;
    }
    g_nearbySortReady.store(false, std::memory_order_release);
    Log("[NearbySort] main-state hook removed\n");
}
