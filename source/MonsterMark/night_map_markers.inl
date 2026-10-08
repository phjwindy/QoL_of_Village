// ---- late-night creature map markers -------------------------------------
//
// CTask_MainMenu_Map rebuilds the visible stamp widgets from the persistent
// CMainGameStatus::mapStampList_ vector.  The detour discovers live
// CCreatureStatus objects through the map owner's native spatial query, then
// supplies a stack-owned TLS pointer view at the exact begin/end register-load
// site only for the synchronous redraw.  The global vector header is never
// modified.  Only actual enabled statuses are shown: the complete-map spawn
// anchors audited by verify_night_map_coverage.py are evidence about coverage,
// never synthetic markers.  No stamp is appended, serialized, or written to a
// save file.

static volatile uintptr_t RVA_NIGHT_MAP_REDRAW = 0x2F52F0;
static volatile uintptr_t RVA_NIGHT_MAP_INITIALIZER_CALL = 0x2F14F5;
static volatile uintptr_t RVA_NIGHT_MAP_VECTOR_READ = 0x2F535C;
static volatile uintptr_t RVA_NIGHT_MAP_VECTOR_RANGE_LOAD = 0x2F536A;
static volatile uintptr_t RVA_NIGHT_MAP_TASK_VTABLE = 0xE42D38;
static volatile uintptr_t RVA_NIGHT_MAP_TASK_COL = 0xEDDB88;
static volatile uintptr_t RVA_NIGHT_MAP_TASK_TYPE = 0x1095748;
static volatile uintptr_t RVA_NIGHT_MAP_TASK_INITIALIZE = 0x2FE900;
static volatile uintptr_t RVA_NIGHT_MAP_STAMP_VTABLE = 0xE2A368;
static volatile uintptr_t RVA_NIGHT_MAP_STAMP_COL = 0xEB67B0;
static volatile uintptr_t RVA_NIGHT_MAP_STAMP_TYPE = 0x1071260;
static volatile uintptr_t RVA_NIGHT_MAP_STAMP_DESTROY = 0x11B710;

static volatile uintptr_t RVA_NIGHT_CREATURE_VTABLE = 0xE27AF8;
static volatile uintptr_t RVA_NIGHT_CREATURE_COL = 0xEB1780;
static volatile uintptr_t RVA_NIGHT_CREATURE_TYPE = 0x106D5A0;
static volatile uintptr_t RVA_NIGHT_CREATURE_DESTROY = 0xECA14;
static volatile uintptr_t RVA_NIGHT_CREATURE_CALLBACK_VTABLE = 0xE3E5D0;
static volatile uintptr_t RVA_NIGHT_CREATURE_CALLBACK_COL = 0xED5A90;
static volatile uintptr_t RVA_NIGHT_CREATURE_CALLBACK_COPY = 0x28E4C0;
static volatile uintptr_t RVA_NIGHT_CREATURE_CALLBACK_INVOKE = 0x28E4B0;
static volatile uintptr_t RVA_NIGHT_CREATURE_CALLBACK_DESTROY = 0x1969B0;
static volatile uintptr_t RVA_NIGHT_MAP_INFORMATION_VTABLE = 0xE354E8;
static volatile uintptr_t RVA_NIGHT_MAP_INFORMATION_COL = 0xEC8330;
static volatile uintptr_t RVA_NIGHT_MAP_INFORMATION_TYPE = 0x1080890;
static volatile uintptr_t RVA_NIGHT_COM_MAP_VTABLE = 0xE314D0;
static volatile uintptr_t RVA_NIGHT_COM_MAP_COL = 0xEC2228;
static volatile uintptr_t RVA_NIGHT_COM_MAP_TYPE = 0x107C640;
static volatile uintptr_t RVA_NIGHT_COM_MAP_SECONDARY_10 = 0xE313B8;
static volatile uintptr_t RVA_NIGHT_COM_MAP_SECONDARY_20 = 0xE314E0;
static volatile uintptr_t RVA_NIGHT_COM_MAP_SECONDARY_248 = 0xE313C8;
static volatile uintptr_t RVA_NIGHT_CREATURE_CONSTRUCTOR = 0xE7400;
static volatile uintptr_t RVA_NIGHT_CREATURE_DATA_BIND = 0xE779E;
static volatile uintptr_t RVA_NIGHT_CREATURE_ID_ACCESS = 0xE80EC;
static volatile uintptr_t RVA_NIGHT_CREATURE_ID_FORMAT = 0xE27588;
static volatile uintptr_t RVA_NIGHT_SPATIAL_SEARCH = 0x194060;
static volatile uintptr_t RVA_NIGHT_RAW_VECTOR_FREE = 0xB8720;
static volatile uintptr_t RVA_NIGHT_STATUS_UNIQUE_ID_GETTER = 0x119590;
static volatile uintptr_t RVA_NIGHT_STATUS_POSITION_GETTER = 0x1195A0;
static volatile uintptr_t RVA_NIGHT_MAP_AREA_RESOLVER = 0x16D860;
static volatile uintptr_t RVA_NIGHT_MAP_AREA_RESOLVER_BOUNDS = 0x16D983;
static volatile uintptr_t RVA_NIGHT_LIVE_AREA_RESOLVER = 0x1D5F00;
static volatile uintptr_t RVA_NIGHT_MAP_AREA_PLAYER_CHAIN = 0x1725A1;
static volatile uintptr_t RVA_NIGHT_MAP_AREA_PLAYER_LIVE_CALL = 0x1725D6;
static volatile uintptr_t RVA_NIGHT_MAP_AREA_PLAYER_CALL = 0x1725EB;
static volatile uintptr_t RVA_NIGHT_MAP_AREA_STATUS_CHAIN = 0xE09C4;
static volatile uintptr_t RVA_NIGHT_MAP_AREA_STATUS_LIVE_CALL = 0xE09EB;
static volatile uintptr_t RVA_NIGHT_MAP_AREA_STATUS_CALL = 0xE0A00;
static volatile uintptr_t RVA_NIGHT_WORLD_TO_MAP = 0x223600;
static volatile uintptr_t RVA_NIGHT_VECTOR_ADD = 0xE7250;
static volatile uintptr_t RVA_NIGHT_WORLD_TO_MAP_CALL = 0x2E9E5B;
static volatile uintptr_t RVA_NIGHT_POSITION_GETTER_CALL = 0x2E9E45;
static volatile uintptr_t RVA_NIGHT_VECTOR_ADD_CALL = 0x2E9EAD;
static volatile uintptr_t RVA_NIGHT_MAP_STAMP_POSITION_COPY = 0x2F5526;
static volatile uintptr_t RVA_NIGHT_MAP_STAMP_NATIVE_LIMIT = 0x2F5512;
static volatile uintptr_t RVA_NIGHT_MAP_STAMP_ALLOCATION = 0x2F5540;
static volatile uintptr_t RVA_NIGHT_MAP_STAMP_LAYOUT_WRITES = 0x2F557F;
static volatile uintptr_t RVA_NIGHT_MAP_STAMP_ATLAS_TYPE = 0x2F5394;
static volatile uintptr_t RVA_NIGHT_MAP_STAMP_SELECTION = 0x2F77C2;
static volatile uintptr_t RVA_NIGHT_STAMP_POSITION_READ = 0x2F5406;
static volatile uintptr_t RVA_NIGHT_STAMP_RENDER_ARGS = 0x2F544E;
static volatile uintptr_t RVA_NIGHT_STAMP_RENDER_CALL = 0x2F5474;
static volatile uintptr_t RVA_NIGHT_STAMP_LOOP_ADVANCE = 0x2F5479;
static volatile uintptr_t RVA_NIGHT_STAMP_RENDERER = 0x740C50;
static volatile uintptr_t RVA_NIGHT_RENDERER_R9_OVERWRITE = 0x740CD2;
static volatile uintptr_t RVA_NIGHT_ENABLED_MEMBERSHIP_PROOF = 0x1916D2;
static volatile uintptr_t RVA_NIGHT_SPATIAL_OWNER_CHAIN = 0xE05D3;
static volatile uintptr_t RVA_NIGHT_SPATIAL_OWNER_LOOKUP_CALL = 0xE05EF;
static volatile uintptr_t RVA_NIGHT_SPATIAL_OWNER_LOOKUP = 0x18E0C0;
static volatile uintptr_t RVA_NIGHT_SPATIAL_ADMISSION_WINDOW = 0xEC576;
static volatile uintptr_t RVA_NIGHT_SPATIAL_ADMISSION_CALL = 0xEC580;
static volatile uintptr_t RVA_NIGHT_SPATIAL_ADMISSION = 0x18DE80;
static volatile uintptr_t RVA_NIGHT_SPATIAL_INDEX_INSERT_WINDOW = 0x18DEDD;
static volatile uintptr_t RVA_NIGHT_SPATIAL_INDEX_INSERT_CALL = 0x18DEEF;
static volatile uintptr_t RVA_NIGHT_SPATIAL_INDEX_INSERT = 0x11A880;
static volatile uintptr_t RVA_NIGHT_MEMBERSHIP_ADMISSION_CALL = 0x191722;
static volatile uintptr_t RVA_NIGHT_MAP_DIVISOR = 0xA2AA88;
static volatile uintptr_t RVA_NIGHT_MAP_SCALE_ONE = 0xE92D54;
static volatile uintptr_t RVA_NIGHT_MAP_OFFSET_X = 0xE932B0;
static volatile uintptr_t RVA_NIGHT_MAP_OFFSET_Y = 0xE93544;
static volatile uintptr_t RVA_NIGHT_PLAYER_STATUS_VTABLE = 0xE2B790;
static volatile uintptr_t RVA_NIGHT_PLAYER_STATUS_COL = 0xEB8AC0;
static volatile uintptr_t RVA_NIGHT_PLAYER_STATUS_TYPE = 0x10730C0;

// --- 0x250910 (StatusSearchByBaseID) --------------------------------------
// Searches the g_gimmickMgr hash map (RVA 0x10DCA20) for a live gimmick
// status object by its baseID (u64 key).  The function internally loads
// g_gimmickMgr from [0x10DCA20], overwriting rcx, then calls the hash
// lookup sub-function 0x17C480.
//
// Prologue (verified from village.exe build 25094764):
//   48 89 5C 24 08   mov [rsp+8], rbx          (5B)
//   57               push rdi                  (1B)
//   B8 50 10 00 00   mov eax, 0x1050           (5B)
//   E8 70 75 73 00   call __chkstk             (5B)
//
// Call convention (from 86 E8 cross-references in the exe):
//   rcx = caller context (saved to rdi, then overwritten internally)
//   rdx = baseID (u64) — the hash key for looking up the gimmick status
//   returns rax = pointer to live gimmick status, or null if not found
// Note: both paths (null/non-null) end with cleanup+ret; the error path
// hits int3 but is unreachable for valid baseIDs.
static volatile uintptr_t RVA_NIGHT_GIMMICK_MGR = 0x1104C80;
static volatile uintptr_t RVA_NIGHT_STATUS_SEARCH_BY_BASE_ID = 0x259300;
static constexpr uintptr_t NIGHT_STATUS_SEARCH_SIG_LENGTH = 11;

// Function type for direct call:
// void* __fastcall(void* ctx, u64 baseID)
using NightStatusSearchByBaseIDFunc = void* (__fastcall *)(void*, u64);
static NightStatusSearchByBaseIDFunc g_nightStatusSearchByBaseID = nullptr;

// Exact anchor search core (book/gear coordinate query).
// 0x1C1A00 is the real function entry: hash-map lookup by anchor name
// inside the CCom_Map, writes world coords + found flag to outBuf.
// (0x1C1760 is NOT a function entry -- it is mid-function inline FP
// code with 0 direct call sites.  v1.0.21 corrected from 0x1C1760
// to 0x1C1A00 after capstone xref analysis.)
// Output buffer layout (at RDX):
//   [0x00..0x0F] float4 world coords (x, y, z, w)
//   [0x10..0x17] void* ref (may be null)
//   [0x20]       bool found (1 = anchor exists, 0 = not found)
static volatile uintptr_t RVA_NIGHT_EXACT_ANCHOR_SEARCH = 0x1CC450;
static constexpr size_t NIGHT_EXACT_ANCHOR_HOOK_LENGTH = 14;

// 0x183B10: hash-map lookup (spatialOwner, uint64_t areaId) -> entry*.
// (0x183B10 was the old RVA; capstone disasm of 0x2657F0 proved the real
//  call is to 0x183B90 at 0x265903.)
static volatile uintptr_t RVA_NIGHT_MAP_HASH_LOOKUP = 0x18E0C0;

// Flag system RVAs (collected-item filtering).
// 0x2A1B0: save data accessor.  void* __fastcall(void* gameDB)
//   gameDB = [0x10DC9B0].  Returns save data object.
//   [save + 0x98] = gimmick list begin, [save + 0xA0] = end.
// 0x10D4950: global save data pointer (g_savePtr).
//   [g_savePtr + 0x208] = flag bitmap base.
//   Flag test: qword at [bitmap_base + (flag_idx>>6)*8 + 0x90],
//   bit = flag_idx & 0x3F.  Set = collected.
static volatile uintptr_t RVA_NIGHT_SAVE_DATA_ACCESSOR = 0x27A40;
static volatile uintptr_t RVA_NIGHT_SAVE_DATA_PTR = 0x10FCBB0;
static volatile uintptr_t RVA_NIGHT_GAME_DB_PTR = 0x1104C10;

static constexpr uintptr_t NIGHT_ROOT_MAP_OWNER_OFFSET = 0x268;
static constexpr uintptr_t NIGHT_ROOT_MAIN_STATUS_OFFSET = 0x208;
static constexpr uintptr_t NIGHT_PLAYER_STATUS_OFFSET = 0x32b8;
static constexpr uintptr_t NIGHT_MAP_INFO_OFFSET = 0x0d8;
static constexpr uintptr_t NIGHT_MAP_SPATIAL_OWNER_OFFSET = 0x030;
static constexpr uintptr_t NIGHT_MAP_SPATIAL_INDEX_OFFSET = 0x6e0;
static constexpr uintptr_t NIGHT_STAMP_VECTOR_OFFSET = 0x35d8;
static constexpr uintptr_t NIGHT_MAP_OBJECT_UNIQUE_ID_OFFSET = 0x0e0;
static constexpr uintptr_t NIGHT_MAP_OBJECT_LIVE_OFFSET = 0x150;
static constexpr uintptr_t NIGHT_MAP_OBJECT_LIVE_VALID_OFFSET = 0x158;
static constexpr uintptr_t NIGHT_CREATURE_POSITION_OFFSET = 0x0f0;
static constexpr uintptr_t NIGHT_CREATURE_ENABLED_OFFSET = 0x130;
static constexpr uintptr_t NIGHT_CREATURE_MAP_LINK_OFFSET = 0x0e8;
static constexpr uintptr_t NIGHT_CREATURE_DATA_OFFSET = 0x290;
static constexpr uintptr_t NIGHT_CREATURE_ID_OFFSET = 0x010;
static constexpr uintptr_t NIGHT_SPATIAL_MAP_LINK_OFFSET = 0x568;
static constexpr uintptr_t NIGHT_SPATIAL_MAP_INFORMATION_OFFSET = 0x548;
static constexpr size_t NIGHT_MAP_REDRAW_HOOK_LENGTH = 18;
static constexpr size_t NIGHT_MAP_VECTOR_RANGE_HOOK_LENGTH = 14;
// This is not derived from one development save.  Native add-stamp code at
// 0x2e6322 computes (end-begin)/8, compares it with immediate 0x64, and skips
// allocation/insertion at >=100.  InstallNightMapMarkers pins that complete
// count/gate branch before accepting this stack-copy bound.
static constexpr size_t NIGHT_MAX_EXISTING_STAMPS = 100;
// The pinned complete map contains 30 Ghost Rat spawn anchors, 11 Treasure Box
// spawn anchors, and 100 Ghost Rat route waypoints across five map resources.
// event_horror_ghost_rat.lub creates at most 3 random Openworld rats plus five
// single-instance/story-gated rats, so its proven live maximum is 8.  Capacity
// 64 also covers the conservative impossible-at-once bound of all 41 audited
// spawn anchors.  A future build that exceeds it is rejected as one redraw;
// the feature never submits a partially truncated marker set.
static constexpr size_t NIGHT_FULL_MAP_RESOURCE_COUNT = 5;
static constexpr size_t NIGHT_FULL_MAP_GHOST_SPAWN_COUNT = 30;
static constexpr size_t NIGHT_FULL_MAP_TREASURE_SPAWN_COUNT = 11;
static constexpr size_t NIGHT_FULL_MAP_GHOST_WAYPOINT_COUNT = 100;
static constexpr size_t NIGHT_EXPECTED_MAX_LIVE_GHOST_RATS = 8;
static constexpr size_t NIGHT_MAX_TARGETS = 64;
static constexpr size_t NIGHT_MAX_SEARCH_RESULTS = 4096;
static constexpr size_t NIGHT_MAX_SEARCH_CAPACITY = 8192;
static constexpr size_t NIGHT_READABLE_REGION_CACHE_SIZE = 128;

// Anchor coordinate cache: 50 machine parts + 62 lost books
static constexpr size_t NIGHT_ANCHOR_CACHE_CAPACITY = 112;
static constexpr int NIGHT_STAMP_TYPE_MACHINE_PART = 3;
static constexpr int NIGHT_STAMP_TYPE_LOST_BOOK = 5;

static constexpr char NIGHT_GHOST_RAT_ID[] =
    "CREATURE_ID_HORROR_GHOST_RAT";
static constexpr char NIGHT_TREASURE_BOX_ID[] =
    "CREATURE_ID_HORROR_TREASURE_BOX";

struct NightRect {
    float minimumX;
    float minimumY;
    float maximumX;
    float maximumY;
};

struct NightPointerVector {
    void** begin;
    void** end;
    void** capacity;
};

struct NightSearchCallback {
    alignas(16) unsigned char storage[0x38];
    void* target;
};

struct alignas(16) NightFakeMapStamp {
    void* vtable;
    LONG references;
    u32 reserved0c;
    int stampType;
    u32 reserved14;
    u64 reserved18;
    float position[4];
};
static_assert(sizeof(NightFakeMapStamp) == 0x30);
static_assert(offsetof(NightFakeMapStamp, stampType) == 0x10);
static_assert(offsetof(NightFakeMapStamp, position) == 0x20);

struct alignas(16) NightMarkerTarget {
    void* status;
    u64 objectId;
    u64 areaId;
    int stampType;
    alignas(16) float worldPosition[4];
    alignas(16) float mapPosition[4];
};
static_assert(sizeof(NightMarkerTarget) == 0x40);
static_assert(alignof(NightMarkerTarget) == 0x10);
static_assert(offsetof(NightMarkerTarget, worldPosition) == 0x20);
static_assert(offsetof(NightMarkerTarget, mapPosition) == 0x30);

enum class NightTargetDecision : unsigned char {
    Accepted,
    AreaUnresolved,
    DifferentArea
};

enum class NightAreaSource : unsigned char {
    None,
    LiveObject,
    MapInformation
};

struct NightTargetObservation {
    u64 objectId;
    u64 areaId;
    int stampType;
    NightTargetDecision decision;
    NightAreaSource areaSource;
    float worldX;
    float worldY;
    float mapX;
    float mapY;
};

struct NightCollectDiagnostics {
    const char* stage;
    size_t rawResults;
    size_t creatureIds;
    size_t statusRejected;
    size_t holderRejected;
    size_t dataRejected;
    size_t idRejected;
    size_t disabledRejected;
    size_t mapRejected;
    size_t identityRejected;
    size_t areaUnresolved;
    size_t differentAreaRejected;
    size_t invalidPositions;
    size_t transformRejected;
    size_t matchingCandidates;
    size_t overflowTargets;
    size_t truncatedTargets;
    u64 elapsedMicroseconds;
    bool hasCoordinateSample;
    float sampleWorldX;
    float sampleWorldY;
    float sampleMapX;
    float sampleMapY;
    char sampleIds[3][64];
    size_t sampleCount;
    u64 readableChecks;
    u64 readableCacheHits;
    u64 readableVirtualQueries;
    size_t readableRegions;
    u64 readableVirtualQueryMicroseconds;
    u64 runtimeMapId;
    u64 playerAreaId;
    NightAreaSource playerAreaSource;
    float playerWorldX;
    float playerWorldY;
    u64 targetStateHash;
    NightTargetObservation observations[NIGHT_MAX_TARGETS];
    size_t observationCount;
    size_t observationTruncated;
};

enum class NightIdReadFailure {
    None,
    Status,
    Holder,
    Data,
    Id
};

static_assert(sizeof(NightSearchCallback) == 0x40,
              "native creature callback layout changed");
static_assert(sizeof(NightFakeMapStamp) == 0x30,
              "native map-stamp layout changed");
static_assert(offsetof(NightFakeMapStamp, stampType) == 0x10,
              "native map-stamp type offset changed");
static_assert(offsetof(NightFakeMapStamp, position) == 0x20,
              "native map-stamp position offset changed");

using NightSpatialSearchFunction = bool (__fastcall *)(
    void*, const NightRect*, NightSearchCallback*, int, int);
using NightRawVectorFreeFunction = void (__fastcall *)(void*, size_t);
using NightMapRedrawFunction = void (__fastcall *)(void*);
using NightVectorTransformFunction = float* (__fastcall *)(
    const float*, float*, const float*);
using NightMapAreaResolverFunction = u64 (__fastcall *)(
    void*, const float*);

static NightSpatialSearchFunction g_nightSpatialSearch = nullptr;
static NightRawVectorFreeFunction g_nightRawVectorFree = nullptr;
static NightMapRedrawFunction g_originalNightMapRedraw = nullptr;
static NightVectorTransformFunction g_nightWorldToMap = nullptr;
static NightVectorTransformFunction g_nightVectorAdd = nullptr;
static NightMapAreaResolverFunction g_nightMapAreaResolver = nullptr;
static NightMapAreaResolverFunction g_nightLiveAreaResolver = nullptr;
static void* g_nightMapRedrawTrampoline = nullptr;
static void* g_nightStampRangeRelay = nullptr;
static bool g_nightMapMarkersAvailable = false;
static std::atomic<bool> g_nightMapMarkersEnabled{true};
static std::atomic<bool> g_nightMapMarkersFaulted{false};
static thread_local bool g_insideNightMapRedraw = false;
static thread_local NightPointerVector* g_nightTransientStampVector = nullptr;
static thread_local bool g_nightTransientStampVectorResolved = false;
static std::atomic<int> g_nightLastGhostRatCount{-1};
static std::atomic<int> g_nightLastTreasureBoxCount{-1};
static std::atomic<size_t> g_nightLastRawResultCount{static_cast<size_t>(-1)};
static std::atomic<size_t> g_nightLastIdCount{static_cast<size_t>(-1)};
static std::atomic<size_t> g_nightLastExistingStampCount{static_cast<size_t>(-1)};
static std::atomic<size_t> g_nightLastExistingStampCapacity{static_cast<size_t>(-1)};
static std::atomic<u64> g_nightLastTargetStateHash{0};
static std::atomic<u64> g_nightRedrawCalls{0};
static std::atomic<DWORD> g_nightRedrawThreadId{0};

// --- AnchorPos hook (book/gear coordinate capture) -------------------------
struct NightAnchorCacheEntry {
    char name[32];
    float worldPos[4];
    bool valid;
};
static NightAnchorCacheEntry g_nightAnchorCache[NIGHT_ANCHOR_CACHE_CAPACITY];
static std::atomic<size_t> g_nightAnchorCacheCount{0};
// 0x1C1A00 signature: void* __fastcall(void* mapObj, void* outBuf, const char* anchorName)
// Returns outBuf (rax = rdx).  outBuf+0x20 = found flag.
using NightExactAnchorSearchFunc = void* (__fastcall *)(void*, void*,
                                                        const char*);
static NightExactAnchorSearchFunc g_originalNightAnchorSearch = nullptr;
static NightExactAnchorSearchFunc g_nightAnchorSearchDirect = nullptr;
// 0x183B10: void* __fastcall(void* spatialOwner, uint64_t areaId)
using NightMapHashLookupFunc = void* (__fastcall *)(void*, uint64_t);
static NightMapHashLookupFunc g_nightMapHashLookup = nullptr;
static std::atomic<u64> g_nightAnchorScanCounter{0};
// v1.0.29: interval=1（每次打开地图都重扫）。之前 60 导致首次扫描拿到
// 部分结果后 59 次不更新，书/齿轮拾取点缺失。昼夜过滤已保证只在夜晚扫描，
// 每次开地图都刷新 0x1C1A00 结果确保拾取点完整。
static constexpr u64 NIGHT_ANCHOR_SCAN_INTERVAL = 1;
// v1.0.25: 记录上次扫描的 areaId，玩家跨区域（地图切换）时强制重扫
static u64 g_nightAnchorScannedAreaId = ~u64{0};

// Progressive (batched) anchor scan: instead of scanning all 112 anchors in
// one frame, process NIGHT_ANCHOR_BATCH_SIZE per frame.
// v1.0.29: batch=112（一帧完成）。每个 0x1C1A00 调用是哈希表查找（微秒级），
// 112 次约 1-2ms，不会丢帧。之前注释说的 11ms 是 NightCollectTargets 空间搜索
// 的开销，不是锚点扫描。改为一帧完成避免开关多次地图才完整显示的问题。
static constexpr size_t NIGHT_ANCHOR_TOTAL = 112;  // 62 books + 50 parts
static constexpr size_t NIGHT_ANCHOR_BATCH_SIZE = 112;
struct NightProgressiveScanState {
    void* mapObj;
    size_t nextIndex;
    size_t cachedCount;
    bool active;
};
static NightProgressiveScanState g_nightProgressive = {};

// Collected-item filter: flag system function pointer and constants.
// The game stores collection state in a global flag bitmap.  Each gimmick
// entry in the save data list has a unique flag_idx; testing the
// corresponding bit tells whether the item has been collected.
using NightSaveDataAccessorFunc = void* (__fastcall *)(void*);
static NightSaveDataAccessorFunc g_nightSaveDataAccessor = nullptr;
static constexpr uintptr_t NIGHT_GIMMICK_LIST_BEGIN_OFFSET = 0x98;
static constexpr uintptr_t NIGHT_GIMMICK_LIST_END_OFFSET = 0xA0;
static constexpr uintptr_t NIGHT_GIMMICK_FLAG_IDX1_OFFSET = 0x258;
static constexpr uintptr_t NIGHT_GIMMICK_FLAG_IDX2_OFFSET = 0x278;
static constexpr uintptr_t NIGHT_GIMMICK_ITEM_NUMBER_OFFSET = 0x288;
static constexpr uintptr_t NIGHT_FLAG_BITMAP_BASE_OFFSET = 0x208;
static constexpr uintptr_t NIGHT_FLAG_BITMAP_QWORD_OFFSET = 0x90;
// Books: 01-62, Machine parts: 01-50.
// v1.0.18: books and machine parts now have SEPARATE collected arrays
// because machine parts are NOT in the save data gimmick list (only books
// are).  Previously a single [64] array was shared, so collecting book #5
// also hid machine part #5.
static constexpr size_t NIGHT_COLLECTED_MAX = 64;
// Book collected set: [1..62] for pop_lost_book01..62
static bool g_nightBookCollected[NIGHT_COLLECTED_MAX] = {};
// Machine part collected set: [1..50] for pop_machine_part01..50
// Populated by probing save data flags at machine-part flag indices.
static bool g_nightGearCollected[NIGHT_COLLECTED_MAX] = {};
// v1.0.20: placed-tonight set RESTORED — capstone disassembly of
// 0x265570 (book placement) proved flagIdx2 IS the placement flag:
//   flagIdx2 == 0      → placed (no flag needed, unconditionally placed)
//   flagIdx2 != 0, bit SET   → placed (tonight placement confirmed)
//   flagIdx2 != 0, bit NOT SET → NOT placed tonight (skip)
// v1.0.18 incorrectly removed this filter based on log analysis that
// showed already-collected books with placed=1.  That was correct —
// collected books CAN have placed=1 (they were placed in a PREVIOUS
// night and have since been collected).  But uncollected books with
// placed=0 were never placed tonight.  The fix: only show anchors that
// are BOTH uncollected AND placed-tonight.
static bool g_nightBookPlaced[NIGHT_COLLECTED_MAX] = {};
static bool g_nightGearPlaced[NIGHT_COLLECTED_MAX] = {};

// v1.0.8: live "spawned tonight" set built from the spatial index.
// The game only places a subset of the 62 books / 50 machine parts each
// night (nightly log verified: books 490400000..490400003 and gears
// 499900000..499900017 appeared in the index; the rest were NOT spawned).
// The flag system cannot tell us which subset is spawned, so we enumerate
// the spatial index once per scan cycle and only display anchors whose
// baseID actually exists.  Slots:
//   [0..61]   = books,  baseID n = 490400000 + (n-1)
//   [62..111] = gears,  baseID n = 499900000 + (n-1)
static constexpr u64 NIGHT_BOOK_BASEID_FIRST = 490400000ULL;
static constexpr u64 NIGHT_GEAR_BASEID_FIRST = 499900000ULL;
static constexpr uintptr_t NIGHT_ALL_OBJECTS_CALLBACK_VTABLE = 0xE3E608;

// v1.0.10: candidate callback vtables paired with the spatial-search
// destroy helper (RVA 0x18C290).  The v1.0.7 F7 brute-force scan proved
// that books (490400000..) and gears (499900000..) ARE reachable through
// the shared spatial index, but only via a SUBSET of these callbacks —
// the single creature callback (0xE1C248) and the "all objects"
// 0xE1C168 both return zero for them.  NightCollectLiveNightItems walks
// this list, keeps whichever callbacks ever produce book/gear objects,
// and only re-scans that cached subset afterwards.
// NIGHT_DIAG_CALLBACKS / NIGHT_DIAG_CALLBACK_COUNT now live near the
// top of this file (next to NightLiveItem) so that both the diagnostic
// scan (F7) and NightCollectLiveNightItems share the same table.
static constexpr uint32_t NIGHT_DIAG_CALLBACKS[] = {
    0xE0ECE0, 0xE1C168, 0xE1C1A0, 0xE1C1D8, 0xE1C210, 0xE1C248,
    0xE1D1D0, 0xE1E278,
    0xE22158, 0xE22350, 0xE22388, 0xE223C0, 0xE22238, 0xE22190,
    0xE221C8, 0xE22200, 0xE22318, 0xE22430, 0xE22468, 0xE224A0,
    0xE22270, 0xE222A8, 0xE222E0, 0xE223F8, 0xE22610, 0xE22648,
    0xE22680, 0xE224F8, 0xE225D8, 0xE227D0, 0xE22808, 0xE22840,
    0xE226B8, 0xE22530, 0xE22568, 0xE225A0, 0xE22798, 0xE226F0,
    0xE22728, 0xE22760,
0xE3D300, 0xE497B8, 0xE49F70, 0xE49FA8, 0xE5DF48, 0xE5DF80,
    0xE69818
};
static constexpr size_t NIGHT_DIAG_CALLBACK_COUNT =
    sizeof(NIGHT_DIAG_CALLBACKS) / sizeof(NIGHT_DIAG_CALLBACKS[0]);

struct NightLiveItem {
    bool found;
    float worldPos[4];
};
static NightLiveItem g_nightLiveItems[NIGHT_ANCHOR_TOTAL] = {};

// Creature collection can query the same committed heap regions repeatedly
// during every synchronous map redraw.  Cache only mapping/protection ranges
// for one NightCollectTargets
// call; object pointers, vtables, enabled state, IDs and map ownership are still
// reread and validated on every redraw.  No status pointer survives the scope.
struct NightReadableRegion {
    uintptr_t begin;
    uintptr_t end;
};

struct NightReadableRegionCache {
    NightReadableRegion regions[NIGHT_READABLE_REGION_CACHE_SIZE];
    size_t count;
    size_t replacement;
    u64 checks;
    u64 cacheHits;
    u64 virtualQueries;
    LONGLONG virtualQueryTicks;
};

static thread_local NightReadableRegionCache* g_nightReadableRegionCache = nullptr;

class NightReadableRegionCacheScope {
public:
    explicit NightReadableRegionCacheScope(NightReadableRegionCache* cache)
        : previous_(g_nightReadableRegionCache) {
        g_nightReadableRegionCache = cache;
    }

    ~NightReadableRegionCacheScope() {
        g_nightReadableRegionCache = previous_;
    }

    NightReadableRegionCacheScope(const NightReadableRegionCacheScope&) = delete;
    NightReadableRegionCacheScope& operator=(
        const NightReadableRegionCacheScope&) = delete;

private:
    NightReadableRegionCache* previous_;
};

static bool NightIsReadable(const void* pointer, size_t size) {
    NightReadableRegionCache* cache = g_nightReadableRegionCache;
    if (!cache) return IsReadable(pointer, size);
    ++cache->checks;
    if (!pointer || size == 0) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(pointer);
    if (start < 0x10000 || start > 0x00007fffffffffffULL ||
        size > UINTPTR_MAX - start) return false;
    const uintptr_t requestedEnd = start + size;
    for (size_t i = 0; i < cache->count; ++i) {
        const NightReadableRegion& region = cache->regions[i];
        if (start >= region.begin && requestedEnd <= region.end) {
            ++cache->cacheHits;
            return true;
        }
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    LARGE_INTEGER queryStarted = {};
    LARGE_INTEGER queryFinished = {};
    QueryPerformanceCounter(&queryStarted);
    ++cache->virtualQueries;
    const SIZE_T querySize = VirtualQuery(pointer, &mbi, sizeof(mbi));
    QueryPerformanceCounter(&queryFinished);
    if (queryFinished.QuadPart >= queryStarted.QuadPart) {
        cache->virtualQueryTicks +=
            queryFinished.QuadPart - queryStarted.QuadPart;
    }
    const DWORD basicProtect = mbi.Protect & 0xff;
    const bool readableProtection =
        basicProtect == PAGE_READONLY ||
        basicProtect == PAGE_READWRITE ||
        basicProtect == PAGE_WRITECOPY ||
        basicProtect == PAGE_EXECUTE_READ ||
        basicProtect == PAGE_EXECUTE_READWRITE ||
        basicProtect == PAGE_EXECUTE_WRITECOPY;
    if (querySize != sizeof(mbi) || mbi.State != MEM_COMMIT ||
        !readableProtection || (mbi.Protect & PAGE_GUARD) != 0) return false;
    const uintptr_t regionBegin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    if (mbi.RegionSize > UINTPTR_MAX - regionBegin) return false;
    const uintptr_t regionEnd = regionBegin + mbi.RegionSize;
    if (start < regionBegin || requestedEnd > regionEnd) return false;

    size_t index = cache->count;
    if (cache->count < NIGHT_READABLE_REGION_CACHE_SIZE) {
        ++cache->count;
    } else {
        index = cache->replacement;
        cache->replacement =
            (cache->replacement + 1) % NIGHT_READABLE_REGION_CACHE_SIZE;
    }
    cache->regions[index] = {regionBegin, regionEnd};
    return true;
}

static size_t NightReadablePrefix(const void* pointer, size_t maximum) {
    if (!pointer || maximum == 0 || !NightIsReadable(pointer, 1)) return 0;
    NightReadableRegionCache* cache = g_nightReadableRegionCache;
    if (!cache) return IsReadable(pointer, maximum) ? maximum : 0;
    const uintptr_t start = reinterpret_cast<uintptr_t>(pointer);
    for (size_t i = 0; i < cache->count; ++i) {
        const NightReadableRegion& region = cache->regions[i];
        if (start >= region.begin && start < region.end) {
            const size_t readable = static_cast<size_t>(region.end - start);
            return readable < maximum ? readable : maximum;
        }
    }
    return 0;
}

static bool NightReadPointer(void* object, uintptr_t offset, void** output) {
    if (!object || !output ||
        !NightIsReadable(reinterpret_cast<void*>(
                        reinterpret_cast<uintptr_t>(object) + offset),
                    sizeof(void*))) return false;
    *output = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(object) + offset);
    return *output != nullptr;
}

static NightPointerVector* __fastcall NightResolveStampVector(void* mainStatus) {
    if (g_nightTransientStampVector) {
        g_nightTransientStampVectorResolved = true;
        return g_nightTransientStampVector;
    }
    return mainStatus ? reinterpret_cast<NightPointerVector*>(
        reinterpret_cast<uintptr_t>(mainStatus) + NIGHT_STAMP_VECTOR_OFFSET)
        : nullptr;
}

static bool NightCheckType(uintptr_t base, uintptr_t vtableRva,
                           uintptr_t colRva, uintptr_t typeRva,
                           const char* typeName) {
    const char* mappedName = reinterpret_cast<const char*>(base + typeRva + 16);
    const size_t length = strlen(typeName) + 1;
    return NightIsReadable(reinterpret_cast<void*>(base + vtableRva - 8), sizeof(void*)) &&
           NightIsReadable(mappedName, length) &&
           *reinterpret_cast<void**>(base + vtableRva - 8) ==
               reinterpret_cast<void*>(base + colRva) &&
           memcmp(mappedName, typeName, length) == 0;
}

static bool NightReadCreatureId(void* status, char output[64],
                                NightIdReadFailure* failure) {
    if (failure) *failure = NightIdReadFailure::None;
    if (!status || !output ||
        !NightIsReadable(status, NIGHT_CREATURE_DATA_OFFSET + sizeof(void*))) {
        if (failure) *failure = NightIdReadFailure::Status;
        return false;
    }
    // The native CCreatureStatus callback has already filtered these results
    // through the class type-ID virtual call.  Concrete creatures may use a
    // derived vtable, so an exact base-vtable equality test is invalid here.
    void* statusVtable = *reinterpret_cast<void**>(status);
    if (!statusVtable || !NightIsReadable(statusVtable, sizeof(void*) * 4)) {
        if (failure) *failure = NightIdReadFailure::Status;
        return false;
    }
    void* dataHolder = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(status) + NIGHT_CREATURE_DATA_OFFSET);
    if (!dataHolder || !NightIsReadable(dataHolder, sizeof(void*))) {
        if (failure) *failure = NightIdReadFailure::Holder;
        return false;
    }
    // Native callback 0x2815FA performs both dereferences:
    // [CCreatureStatus+0x290] -> holder, [holder] -> CreatureData.
    void* data = *reinterpret_cast<void**>(dataHolder);
    if (!data ||
        !NightIsReadable(reinterpret_cast<void*>(
                        reinterpret_cast<uintptr_t>(data) +
                        NIGHT_CREATURE_ID_OFFSET),
                    sizeof(void*))) {
        if (failure) *failure = NightIdReadFailure::Data;
        return false;
    }
    // Native CCreatureStatus code at 0x0E10AC follows this exact chain and
    // passes [CreatureData+0x10] as the %s argument of "pop_sensor_%s".
    // It is the creature ID.  The old +0x450/+0x458 interpretation belonged
    // to an unrelated serialized class and rejected every live creature.
    const char* text = *reinterpret_cast<const char**>(
        reinterpret_cast<uintptr_t>(data) + NIGHT_CREATURE_ID_OFFSET);
    if (!text) {
        if (failure) *failure = NightIdReadFailure::Id;
        return false;
    }
    // Resolve the readable prefix through the redraw-local region cache.  This
    // still refuses to cross a committed-region boundary, but repeated IDs and
    // object fields in the same heap region no longer repeat VirtualQuery.
    const size_t limit = NightReadablePrefix(text, 64);
    for (size_t i = 0; i < limit; ++i) {
        output[i] = text[i];
        if (output[i] == '\0') {
            if (i == 0 && failure) *failure = NightIdReadFailure::Id;
            return i != 0;
        }
        if (static_cast<unsigned char>(output[i]) < 0x20 ||
            static_cast<unsigned char>(output[i]) > 0x7e) {
            output[0] = '\0';
            if (failure) *failure = NightIdReadFailure::Id;
            return false;
        }
    }
    output[0] = '\0';
    if (failure) *failure = NightIdReadFailure::Id;
    return false;
}

static bool NightReadMapIdentity(void* object, uintptr_t linkOffset,
                                 void** identity) {
    if (!object || !identity ||
        !NightIsReadable(reinterpret_cast<void*>(
                        reinterpret_cast<uintptr_t>(object) + linkOffset),
                    sizeof(void*))) return false;
    void* outer = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(object) + linkOffset);
    if (!outer) {
        *identity = nullptr;
        return true;
    }
    if (!NightIsReadable(outer, sizeof(void*))) return false;
    void* inner = *reinterpret_cast<void**>(outer);
    if (!inner || !NightIsReadable(inner, sizeof(void*))) return false;
    *identity = *reinterpret_cast<void**>(inner);
    return true;
}

static bool NightReadStableObjectId(void* status, u64* objectId) {
    // Native leaf getter 0x1118a0 is exactly `mov rax,[rcx+0xe0]; ret`.
    // InstallNightMapMarkers pins those complete eight bytes before this
    // scalar is used to correlate movement across redraws.
    if (!status || !objectId ||
        !NightIsReadable(reinterpret_cast<void*>(
                        reinterpret_cast<uintptr_t>(status) +
                        NIGHT_MAP_OBJECT_UNIQUE_ID_OFFSET),
                    sizeof(u64))) return false;
    *objectId = *reinterpret_cast<const u64*>(
        reinterpret_cast<uintptr_t>(status) +
        NIGHT_MAP_OBJECT_UNIQUE_ID_OFFSET);
    return *objectId != 0;
}

static bool NightResolveAreaId(void* status, const float* position,
                               u64* areaId, NightAreaSource* areaSource) {
    if (areaSource) *areaSource = NightAreaSource::None;
    if (!status || !position || !areaId || !g_nightMapAreaResolver ||
        !g_nightLiveAreaResolver ||
        !NightIsReadable(status,
                         NIGHT_MAP_OBJECT_LIVE_VALID_OFFSET + sizeof(void*)))
        return false;
    void* liveValidity = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(status) +
        NIGHT_MAP_OBJECT_LIVE_VALID_OFFSET);
    if (liveValidity) {
        if (!NightIsReadable(liveValidity, 1)) return false;
        if (*reinterpret_cast<const unsigned char*>(liveValidity) != 0) {
            void* liveObject = *reinterpret_cast<void**>(
                reinterpret_cast<uintptr_t>(status) +
                NIGHT_MAP_OBJECT_LIVE_OFFSET);
            if (liveObject) {
                // Both audited native callers use this exact live-object path
                // first: RCX=live object, RDX=&status+0xf0.  The callee reads
                // liveObject+0x30, so reject an unreadable object instead of
                // attempting the map-information fallback on corrupt state.
                if (!NightIsReadable(liveObject, 0x38)) return false;
                u64 resolved = 0;
                __try {
                    resolved = g_nightLiveAreaResolver(liveObject, position);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    resolved = 0;
                }
                if (resolved == 0) return false;
                *areaId = resolved;
                if (areaSource) *areaSource = NightAreaSource::LiveObject;
                return true;
            }
        }
    }
    void* mapHolder = nullptr;
    if (!NightReadPointer(status, NIGHT_CREATURE_MAP_LINK_OFFSET,
                          &mapHolder)) return false;
    u64 resolved = 0;
    __try {
        resolved = g_nightMapAreaResolver(mapHolder, position);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        resolved = 0;
    }
    if (resolved == 0) return false;
    *areaId = resolved;
    if (areaSource) *areaSource = NightAreaSource::MapInformation;
    return true;
}

static bool NightResolvePlayerArea(uintptr_t base, void* root, u64* areaId,
                                   NightAreaSource* areaSource,
                                   float* worldX, float* worldY) {
    if (!root || !areaId || !areaSource || !worldX || !worldY) return false;
    void* save = nullptr;
    void* playerStatus = nullptr;
    if (!NightReadPointer(root, NIGHT_ROOT_MAIN_STATUS_OFFSET, &save) ||
        !NightReadPointer(save, NIGHT_PLAYER_STATUS_OFFSET, &playerStatus) ||
        !NightIsReadable(playerStatus,
                         NIGHT_CREATURE_POSITION_OFFSET + sizeof(float) * 4))
        return false;
    // 跳过 vtable 校验（与 ChestSort 一致），不同 build 的派生类 vtable 可能偏移
    const float* position = reinterpret_cast<const float*>(
        reinterpret_cast<uintptr_t>(playerStatus) +
        NIGHT_CREATURE_POSITION_OFFSET);
    if (!std::isfinite(position[0]) || !std::isfinite(position[1]) ||
        !NightResolveAreaId(playerStatus, position, areaId, areaSource))
        return false;
    *worldX = position[0];
    *worldY = position[1];
    return true;
}

static u64 NightExtendStateHash(u64 hash, const void* bytes, size_t size) {
    const unsigned char* current =
        reinterpret_cast<const unsigned char*>(bytes);
    for (size_t index = 0; index < size; ++index) {
        hash ^= current[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static void NightRecordObservation(NightCollectDiagnostics* diagnostics,
                                   u64 objectId, u64 areaId, int stampType,
                                   NightTargetDecision decision,
                                   NightAreaSource areaSource,
                                   const float world[4],
                                   const float map[4]) {
    if (!diagnostics) return;
    u64 hash = diagnostics->targetStateHash;
    hash = NightExtendStateHash(hash, &objectId, sizeof(objectId));
    hash = NightExtendStateHash(hash, &areaId, sizeof(areaId));
    hash = NightExtendStateHash(hash, &stampType, sizeof(stampType));
    const unsigned char decisionByte =
        static_cast<unsigned char>(decision);
    hash = NightExtendStateHash(hash, &decisionByte, sizeof(decisionByte));
    const unsigned char areaSourceByte =
        static_cast<unsigned char>(areaSource);
    hash = NightExtendStateHash(hash, &areaSourceByte,
                                sizeof(areaSourceByte));
    hash = NightExtendStateHash(hash, world, sizeof(float) * 2);
    diagnostics->targetStateHash = hash;
    if (diagnostics->observationCount >= NIGHT_MAX_TARGETS) {
        ++diagnostics->observationTruncated;
        return;
    }
    NightTargetObservation& observation =
        diagnostics->observations[diagnostics->observationCount++];
    observation.objectId = objectId;
    observation.areaId = areaId;
    observation.stampType = stampType;
    observation.decision = decision;
    observation.areaSource = areaSource;
    observation.worldX = world[0];
    observation.worldY = world[1];
    observation.mapX = map ? map[0] : 0.0f;
    observation.mapY = map ? map[1] : 0.0f;
}

static bool NightTransformWorldToMap(const float world[4], float map[4]) {
    if (!world || !map || !g_nightWorldToMap || !g_nightVectorAdd)
        return false;
    const uintptr_t base =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const float* divisor = reinterpret_cast<const float*>(
        base + RVA_NIGHT_MAP_DIVISOR);
    alignas(16) const float offset[4] = {
        *reinterpret_cast<const float*>(base + RVA_NIGHT_MAP_OFFSET_X),
        *reinterpret_cast<const float*>(base + RVA_NIGHT_MAP_OFFSET_Y),
        0.0f,
        0.0f
    };
    // 0x218f44 is legacy SSE `mulps xmm1, xmmword ptr [rcx]`; unlike the
    // surrounding movups instructions, its memory operand must be 16-byte
    // aligned.  Copy at this native ABI boundary even though our current
    // target structure also guarantees aligned storage.
    alignas(16) float alignedWorld[4] = {};
    alignas(16) float scaled[4] = {};
    memcpy(alignedWorld, world, sizeof(alignedWorld));
    __try {
        g_nightWorldToMap(alignedWorld, scaled, divisor);
        g_nightVectorAdd(scaled, map, offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    // Native 0x2e6336..0x2e634c packs only map X/Y into xmm6 after first
    // zeroing it, so CMapStampStatus+0x20 is exactly {X,Y,0,0}.
    map[2] = 0.0f;
    map[3] = 0.0f;
    return std::isfinite(map[0]) && std::isfinite(map[1]);
}

static bool NightIsGhostRatId(const char* id) {
    // This is the exact CreatureData+0x10 value observed in the isolated
    // live log and present in data.dat.  Do not accept guessed aliases.
    return id && strcmp(id, NIGHT_GHOST_RAT_ID) == 0;
}

static bool NightIsTreasureBoxId(const char* id) {
    return id && strcmp(id, NIGHT_TREASURE_BOX_ID) == 0;
}

static void NightReleaseSearchVector(NightPointerVector* vector) {
    if (!vector || !vector->begin) return;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(vector->begin);
    const uintptr_t end = reinterpret_cast<uintptr_t>(vector->end);
    const uintptr_t capacity = reinterpret_cast<uintptr_t>(vector->capacity);
    const uintptr_t bytes = capacity >= begin ? capacity - begin : 0;
    if (end >= begin && capacity >= end && bytes % sizeof(void*) == 0 &&
        bytes <= NIGHT_MAX_SEARCH_CAPACITY * sizeof(void*) &&
        g_nightRawVectorFree) {
        __try {
            g_nightRawVectorFree(vector->begin, static_cast<size_t>(bytes));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[NightMapMarkers] raw vector free SEH; begin=%p\n",
                vector->begin);
        }
    } else {
        Log("[NightMapMarkers] invalid search vector retained safely begin=%p "
            "end=%p cap=%p\n", vector->begin, vector->end, vector->capacity);
    }
    *vector = {};
}

static size_t NightCollectTargets(NightMarkerTarget targets[NIGHT_MAX_TARGETS],
                                  int* ghostRatCount, int* treasureBoxCount,
                                  NightCollectDiagnostics* diagnostics) {
    if (ghostRatCount) *ghostRatCount = 0;
    if (treasureBoxCount) *treasureBoxCount = 0;
    if (diagnostics) {
        *diagnostics = {};
        diagnostics->stage = "start";
        diagnostics->targetStateHash = 14695981039346656037ULL;
    }
    if (!g_nightSpatialSearch || !g_nightRawVectorFree ||
        !g_nightMapAreaResolver || !g_nightLiveAreaResolver) {
        if (diagnostics) diagnostics->stage = "native_functions_unavailable";
        return 0;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    void* root = nullptr;
    void* mapOwner = nullptr;
    void* mapInfo = nullptr;
    void* spatialOwner = nullptr;
    if (!NightReadPointer(reinterpret_cast<void*>(base + RVA_GAME_ROOT), 0, &root)) {
        if (diagnostics) diagnostics->stage = "root_unavailable";
        return 0;
    }
    u64 playerAreaId = 0;
    NightAreaSource playerAreaSource = NightAreaSource::None;
    float playerWorldX = 0.0f;
    float playerWorldY = 0.0f;
    if (!NightResolvePlayerArea(base, root, &playerAreaId,
                                &playerAreaSource,
                                &playerWorldX, &playerWorldY)) {
        if (diagnostics) diagnostics->stage = "player_area_unavailable";
        return 0;
    }
    if (diagnostics) {
        diagnostics->playerAreaId = playerAreaId;
        diagnostics->playerAreaSource = playerAreaSource;
        diagnostics->playerWorldX = playerWorldX;
        diagnostics->playerWorldY = playerWorldY;
        diagnostics->targetStateHash = NightExtendStateHash(
            diagnostics->targetStateHash, &playerAreaId,
            sizeof(playerAreaId));
    }
    if (!NightReadPointer(root, NIGHT_ROOT_MAP_OWNER_OFFSET, &mapOwner)) {
        if (diagnostics) diagnostics->stage = "map_owner_unavailable";
        return 0;
    }
    if (!NightReadPointer(mapOwner, NIGHT_MAP_INFO_OFFSET, &mapInfo)) {
        if (diagnostics) diagnostics->stage = "map_info_unavailable";
        return 0;
    }
    if (!NightReadPointer(mapInfo, NIGHT_MAP_SPATIAL_OWNER_OFFSET, &spatialOwner)) {
        if (diagnostics) diagnostics->stage = "spatial_owner_unavailable";
        return 0;
    }
    if (!NightIsReadable(mapInfo, sizeof(void*))) {
        if (diagnostics) diagnostics->stage = "map_info_type_mismatch";
        return 0;
    }
    if (!NightIsReadable(spatialOwner,
                    NIGHT_SPATIAL_MAP_INFORMATION_OFFSET + sizeof(void*))) {
        if (diagnostics) diagnostics->stage = "spatial_owner_type_mismatch";
        return 0;
    }
    void* spatialIndex = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(spatialOwner) + NIGHT_MAP_SPATIAL_INDEX_OFFSET);
    if (!NightIsReadable(spatialIndex, 0x48)) {
        if (diagnostics) diagnostics->stage = "spatial_index_unreadable";
        return 0;
    }
    NightRect bounds = *reinterpret_cast<const NightRect*>(
        reinterpret_cast<uintptr_t>(spatialIndex) + 0x10);
    if (!std::isfinite(bounds.minimumX) || !std::isfinite(bounds.minimumY) ||
        !std::isfinite(bounds.maximumX) || !std::isfinite(bounds.maximumY) ||
        bounds.minimumX > bounds.maximumX || bounds.minimumY > bounds.maximumY) {
        if (diagnostics) diagnostics->stage = "spatial_bounds_invalid";
        return 0;
    }

    NightPointerVector results = {};
    u64 filterId = 0;
    NightSearchCallback callback = {};
    *reinterpret_cast<void**>(callback.storage) =
        reinterpret_cast<void*>(base + RVA_NIGHT_CREATURE_CALLBACK_VTABLE);
    *reinterpret_cast<u64**>(callback.storage + 0x08) = &filterId;
    *reinterpret_cast<NightPointerVector**>(callback.storage + 0x10) = &results;
    callback.target = callback.storage;
    __try {
        g_nightSpatialSearch(spatialIndex, &bounds, &callback, -1, -1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    if (callback.target) {
        void** vtable = NightIsReadable(callback.target, sizeof(void*))
            ? *reinterpret_cast<void***>(callback.target) : nullptr;
        if (vtable && NightIsReadable(vtable, sizeof(void*) * 5)) {
            using DestroyFunction = void (__fastcall*)(void*, bool);
            __try {
                reinterpret_cast<DestroyFunction>(vtable[4])(
                    callback.target, callback.target != callback.storage);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        } else {
            Log("[NightMapMarkers] callback vtable unreadable after search; "
                "skipping destroy\n");
        }
        callback.target = nullptr;
    }

    const uintptr_t begin = reinterpret_cast<uintptr_t>(results.begin);
    const uintptr_t end = reinterpret_cast<uintptr_t>(results.end);
    const uintptr_t capacity = reinterpret_cast<uintptr_t>(results.capacity);
    const bool vectorOk = (!begin && !end && !capacity) ||
        (begin && end >= begin && capacity >= end &&
         (end - begin) % sizeof(void*) == 0 &&
         (capacity - begin) % sizeof(void*) == 0);
    const size_t resultCount = vectorOk && begin
        ? static_cast<size_t>((end - begin) / sizeof(void*)) : 0;
    if (diagnostics) diagnostics->rawResults = resultCount;
    if (!vectorOk || resultCount > NIGHT_MAX_SEARCH_RESULTS ||
        (resultCount &&
         !NightIsReadable(results.begin, resultCount * sizeof(void*)))) {
        Log("[NightMapMarkers] creature search vector rejected count=%zu\n",
            resultCount);
        if (diagnostics) diagnostics->stage = "search_vector_rejected";
        NightReleaseSearchVector(&results);
        return 0;
    }

    void* spatialMapIdentity = nullptr;
    if (!NightReadMapIdentity(spatialOwner, NIGHT_SPATIAL_MAP_LINK_OFFSET,
                              &spatialMapIdentity)) {
        if (diagnostics) diagnostics->stage = "spatial_identity_unreadable";
        NightReleaseSearchVector(&results);
        return 0;
    }
    if (diagnostics) {
        diagnostics->runtimeMapId =
            static_cast<u64>(reinterpret_cast<uintptr_t>(spatialMapIdentity));
    }

    size_t targetCount = 0;
    for (size_t i = 0; i < resultCount; ++i) {
        void* status = results.begin[i];
        if (!status ||
            !NightIsReadable(reinterpret_cast<void*>(
                            reinterpret_cast<uintptr_t>(status) +
                            NIGHT_CREATURE_ENABLED_OFFSET), 1)) {
            if (diagnostics) ++diagnostics->statusRejected;
            continue;
        }
        if (*reinterpret_cast<const unsigned char*>(
                reinterpret_cast<uintptr_t>(status) +
                NIGHT_CREATURE_ENABLED_OFFSET) == 0) {
            if (diagnostics) ++diagnostics->disabledRejected;
            continue;
        }
        // Native registration at 0x188f0f..0x188f40 compares these exact
        // identities before inserting a status into spatialOwner+0x6e0.
        // Repeat it here to reject a stale node or an object from another map.
        void* statusMapIdentity = nullptr;
        if (!NightReadMapIdentity(status, NIGHT_CREATURE_MAP_LINK_OFFSET,
                                  &statusMapIdentity) ||
            statusMapIdentity != spatialMapIdentity) {
            if (diagnostics) ++diagnostics->mapRejected;
            continue;
        }
        char creatureId[64] = {};
        NightIdReadFailure readFailure = NightIdReadFailure::None;
        if (!NightReadCreatureId(status, creatureId, &readFailure)) {
            if (diagnostics) {
                switch (readFailure) {
                    case NightIdReadFailure::Status:
                        ++diagnostics->statusRejected;
                        break;
                    case NightIdReadFailure::Holder:
                        ++diagnostics->holderRejected;
                        break;
                    case NightIdReadFailure::Data:
                        ++diagnostics->dataRejected;
                        break;
                    case NightIdReadFailure::Id:
                        ++diagnostics->idRejected;
                        break;
                    default:
                        break;
                }
            }
            continue;
        }
        if (diagnostics) {
            ++diagnostics->creatureIds;
            if (diagnostics->sampleCount < 3) {
                strcpy_s(diagnostics->sampleIds[diagnostics->sampleCount],
                         creatureId);
                ++diagnostics->sampleCount;
            }
        }
        int stampType = -1;
        bool isGhostRat = false;
        if (NightIsGhostRatId(creatureId)) {
            stampType = 0; // first native stamp atlas cell
            isGhostRat = true;
        } else if (NightIsTreasureBoxId(creatureId)) {
            stampType = 1; // second native stamp atlas cell
        } else {
            // v1.0.19: one-shot diagnostic — dump ALL creatures in the
            // spatial index (not just ghost_rat/treasure_box) so we can
            // see if book/gear live entities exist and what their
            // creature IDs look like.
            // v1.0.20: enhanced dump — add +0x150 liveObj, +0x158 liveVal,
            // +0x280 areaId, +0x288 itemNum for fake_item analysis.
            {
                static bool s_dumpedAll = false;
                if (!s_dumpedAll) {
                    u64 bid = 0;
                    void* holder = nullptr;
                    if (NightReadPointer(status, 0x240, &holder) && holder) {
                        void* data = nullptr;
                        if (NightReadPointer(holder, 0, &data) && data &&
                            NightIsReadable(data, sizeof(u64))) {
                            bid = *reinterpret_cast<const u64*>(data);
                        }
                    }
                    const float* pos = reinterpret_cast<const float*>(
                        reinterpret_cast<uintptr_t>(status) +
                        NIGHT_CREATURE_POSITION_OFFSET);
                    float px = 0, py = 0;
                    if (NightIsReadable(pos, sizeof(float) * 2)) {
                        px = pos[0]; py = pos[1];
                    }
                    uint8_t enByte = 0xFF;
                    if (NightIsReadable(reinterpret_cast<void*>(
                            reinterpret_cast<uintptr_t>(status)+0x130), 1))
                        enByte = *reinterpret_cast<const uint8_t*>(
                            reinterpret_cast<uintptr_t>(status)+0x130);
                    // v1.0.20: dump extra fields for fake_item analysis
                    void* liveObj = nullptr;
                    void* liveVal = nullptr;
                    NightReadPointer(status, 0x150, &liveObj);
                    NightReadPointer(status, 0x158, &liveVal);
                    uint64_t areaId = 0;
                    if (NightIsReadable(reinterpret_cast<void*>(
                            reinterpret_cast<uintptr_t>(status)+0x280),
                            sizeof(uint64_t)))
                        areaId = *reinterpret_cast<const uint64_t*>(
                            reinterpret_cast<uintptr_t>(status)+0x280);
                    int32_t itemNum = -1;
                    if (NightIsReadable(reinterpret_cast<void*>(
                            reinterpret_cast<uintptr_t>(status)+0x288),
                            sizeof(int32_t)))
                        itemNum = *reinterpret_cast<const int32_t*>(
                            reinterpret_cast<uintptr_t>(status)+0x288);
                    Log("[NightMapMarkers][diag] CREATURE[%zu] id=\"%s\" "
                        "baseID=%llu en=%u pos=(%.1f,%.1f) s=%p "
                        "liveObj=%p liveVal=%p area=%llu itemNum=%d\n",
                        i, creatureId,
                        static_cast<unsigned long long>(bid),
                        static_cast<unsigned>(enByte),
                        px, py, status,
                        liveObj, liveVal,
                        static_cast<unsigned long long>(areaId),
                        itemNum);
                }
            }
            continue;
        }
        // Search results are normally unique, but de-duplicate against the
        // complete preceding native result range rather than only the bounded
        // output array.  This keeps overflow accounting exact even if a future
        // spatial implementation returns one status more than once.
        bool duplicate = false;
        for (size_t j = 0; j < i; ++j) {
            if (results.begin[j] == status) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        u64 objectId = 0;
        if (!NightReadStableObjectId(status, &objectId)) {
            if (diagnostics) ++diagnostics->identityRejected;
            continue;
        }
        const float* position = reinterpret_cast<const float*>(
            reinterpret_cast<uintptr_t>(status) + NIGHT_CREATURE_POSITION_OFFSET);
        if (!NightIsReadable(position, sizeof(float) * 4) ||
            !std::isfinite(position[0]) || !std::isfinite(position[1])) {
            if (diagnostics) ++diagnostics->invalidPositions;
            continue;
        }
        alignas(16) float worldPosition[4] = {};
        alignas(16) float mapPosition[4] = {};
        memcpy(worldPosition, position, sizeof(worldPosition));
        if (diagnostics) ++diagnostics->matchingCandidates;
        u64 targetAreaId = 0;
        NightAreaSource targetAreaSource = NightAreaSource::None;
        if (!NightResolveAreaId(status, worldPosition, &targetAreaId,
                                &targetAreaSource)) {
            if (diagnostics) {
                ++diagnostics->areaUnresolved;
                NightRecordObservation(
                    diagnostics, objectId, 0, stampType,
                    NightTargetDecision::AreaUnresolved,
                    NightAreaSource::None, worldPosition,
                    nullptr);
            }
            continue;
        }
        if (targetAreaId != playerAreaId) {
            if (diagnostics) {
                ++diagnostics->differentAreaRejected;
                NightRecordObservation(
                    diagnostics, objectId, targetAreaId, stampType,
                    NightTargetDecision::DifferentArea, targetAreaSource,
                    worldPosition,
                    nullptr);
            }
            continue;
        }
        if (!NightTransformWorldToMap(worldPosition, mapPosition)) {
            if (diagnostics) ++diagnostics->transformRejected;
            continue;
        }
        if (diagnostics) {
            NightRecordObservation(
                diagnostics, objectId, targetAreaId, stampType,
                NightTargetDecision::Accepted, targetAreaSource,
                worldPosition, mapPosition);
        }
        if (targetCount >= NIGHT_MAX_TARGETS) {
            if (diagnostics) {
                ++diagnostics->overflowTargets;
                ++diagnostics->truncatedTargets;
            }
            continue;
        }
        targets[targetCount].status = status;
        targets[targetCount].objectId = objectId;
        targets[targetCount].areaId = targetAreaId;
        targets[targetCount].stampType = stampType;
        memcpy(targets[targetCount].worldPosition, worldPosition,
               sizeof(worldPosition));
        memcpy(targets[targetCount].mapPosition, mapPosition,
               sizeof(mapPosition));
        if (diagnostics && !diagnostics->hasCoordinateSample) {
            diagnostics->hasCoordinateSample = true;
            diagnostics->sampleWorldX = targets[targetCount].worldPosition[0];
            diagnostics->sampleWorldY = targets[targetCount].worldPosition[1];
            diagnostics->sampleMapX = targets[targetCount].mapPosition[0];
            diagnostics->sampleMapY = targets[targetCount].mapPosition[1];
        }
        ++targetCount;
        if (isGhostRat) {
            if (ghostRatCount) ++*ghostRatCount;
        } else if (treasureBoxCount) {
            ++*treasureBoxCount;
        }
    }
    if (diagnostics && diagnostics->overflowTargets != 0) {
        // Partial coverage is worse than a native-only redraw.  Preserve the
        // counts in diagnostics, but publish no transient marker view.
        diagnostics->stage = "target_capacity_exceeded";
        NightReleaseSearchVector(&results);
        return 0;
    }
    if (diagnostics) diagnostics->stage = "complete";
    NightReleaseSearchVector(&results);
    return targetCount;
}

#ifdef MONSTERMARK_LOGGING
static u64 NightLogRedrawDiagnostics(const NightCollectDiagnostics& diagnostics,
                                     size_t targetCount, int ghostRatCount,
                                     int treasureBoxCount,
                                     bool persistentVectorInspected,
                                     size_t existingStampCount,
                                     size_t existingStampCapacity) {
    const auto areaSourceText = [](NightAreaSource source) -> const char* {
        switch (source) {
            case NightAreaSource::LiveObject:
                return "live_object_0x1cc410";
            case NightAreaSource::MapInformation:
                return "map_information_0x165820";
            default:
                return "unresolved";
        }
    };
    const u64 call = g_nightRedrawCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    const size_t previousRaw = g_nightLastRawResultCount.exchange(
        diagnostics.rawResults, std::memory_order_relaxed);
    const size_t previousIds = g_nightLastIdCount.exchange(
        diagnostics.creatureIds, std::memory_order_relaxed);
    const int previousGhost = g_nightLastGhostRatCount.exchange(
        ghostRatCount, std::memory_order_relaxed);
    const int previousTreasure = g_nightLastTreasureBoxCount.exchange(
        treasureBoxCount, std::memory_order_relaxed);
    const size_t reportedExisting = persistentVectorInspected
        ? existingStampCount : static_cast<size_t>(-1);
    const size_t reportedCapacity = persistentVectorInspected
        ? existingStampCapacity : static_cast<size_t>(-1);
    const size_t previousExisting = g_nightLastExistingStampCount.exchange(
        reportedExisting, std::memory_order_relaxed);
    const size_t previousCapacity = g_nightLastExistingStampCapacity.exchange(
        reportedCapacity, std::memory_order_relaxed);
    const u64 previousTargetState = g_nightLastTargetStateHash.exchange(
        diagnostics.targetStateHash, std::memory_order_relaxed);
    if (call > 12 && previousRaw == diagnostics.rawResults &&
        previousIds == diagnostics.creatureIds &&
        previousGhost == ghostRatCount &&
        previousTreasure == treasureBoxCount &&
        previousExisting == reportedExisting &&
        previousCapacity == reportedCapacity &&
        previousTargetState == diagnostics.targetStateHash) return 0;

    Log("[NightMapMarkers] redraw_observed call=%llu stage=%s raw=%zu "
        "id_readable=%zu rejected=status:%zu/holder:%zu/data:%zu/id:%zu "
        "disabled:%zu/map:%zu/identity:%zu/area_unresolved:%zu/"
        "different_area:%zu invalid_position=%zu transform_rejected=%zu "
        "ghost_rat=%d treasure_box=%d matched=%zu candidates=%zu "
        "capacity=%zu overflow=%zu truncated=%zu elapsed_us=%llu "
        "persistent_vector_inspected=%d existing_stamps=%zu "
        "existing_capacity=%zu native_existing_limit=%zu "
        "runtime_map_id=%llu player_area_id=%llu player_area_source=%s "
        "player_world=(%.2f,%.2f) target_state_hash=%llu "
        "observations=%zu observation_truncated=%zu "
        "readable=checks:%llu/cache_hits:%llu/virtual_queries:%llu/regions:%zu/"
        "vq_us:%llu "
        "coord=world(%.2f,%.2f)->map(%.2f,%.2f) samples=%s|%s|%s\n",
        static_cast<unsigned long long>(call),
        diagnostics.stage ? diagnostics.stage : "unknown",
        diagnostics.rawResults, diagnostics.creatureIds,
        diagnostics.statusRejected, diagnostics.holderRejected,
        diagnostics.dataRejected, diagnostics.idRejected,
        diagnostics.disabledRejected, diagnostics.mapRejected,
        diagnostics.identityRejected, diagnostics.areaUnresolved,
        diagnostics.differentAreaRejected,
        diagnostics.invalidPositions, diagnostics.transformRejected,
        ghostRatCount, treasureBoxCount, targetCount,
        diagnostics.matchingCandidates, NIGHT_MAX_TARGETS,
        diagnostics.overflowTargets, diagnostics.truncatedTargets,
        static_cast<unsigned long long>(diagnostics.elapsedMicroseconds),
        persistentVectorInspected ? 1 : 0,
        persistentVectorInspected ? existingStampCount : 0,
        persistentVectorInspected ? existingStampCapacity : 0,
        NIGHT_MAX_EXISTING_STAMPS,
        static_cast<unsigned long long>(diagnostics.runtimeMapId),
        static_cast<unsigned long long>(diagnostics.playerAreaId),
        areaSourceText(diagnostics.playerAreaSource),
        diagnostics.playerWorldX, diagnostics.playerWorldY,
        static_cast<unsigned long long>(diagnostics.targetStateHash),
        diagnostics.observationCount, diagnostics.observationTruncated,
        static_cast<unsigned long long>(diagnostics.readableChecks),
        static_cast<unsigned long long>(diagnostics.readableCacheHits),
        static_cast<unsigned long long>(diagnostics.readableVirtualQueries),
        diagnostics.readableRegions,
        static_cast<unsigned long long>(
            diagnostics.readableVirtualQueryMicroseconds),
        diagnostics.hasCoordinateSample ? diagnostics.sampleWorldX : 0.0f,
        diagnostics.hasCoordinateSample ? diagnostics.sampleWorldY : 0.0f,
        diagnostics.hasCoordinateSample ? diagnostics.sampleMapX : 0.0f,
        diagnostics.hasCoordinateSample ? diagnostics.sampleMapY : 0.0f,
        diagnostics.sampleCount > 0 ? diagnostics.sampleIds[0] : "-",
        diagnostics.sampleCount > 1 ? diagnostics.sampleIds[1] : "-",
        diagnostics.sampleCount > 2 ? diagnostics.sampleIds[2] : "-");
    for (size_t index = 0; index < diagnostics.observationCount; ++index) {
        const NightTargetObservation& observation =
            diagnostics.observations[index];
        const char* type = observation.stampType == 0
            ? "ghost_rat" : "treasure_box";
        const char* decision = observation.decision ==
                NightTargetDecision::Accepted
            ? "accepted"
            : (observation.decision == NightTargetDecision::DifferentArea
                ? "different_area" : "area_unresolved");
        Log("[NightMapMarkers] target_observed redraw=%llu index=%zu "
            "object_id=%llu type=%s area_id=%llu player_area_id=%llu "
            "world=(%.2f,%.2f) map=(%.2f,%.2f) decision=%s "
            "position_source=status_plus_0xf0 area_source=%s "
            "cross_frame_pointer_cache=0\n",
            static_cast<unsigned long long>(call), index,
            static_cast<unsigned long long>(observation.objectId), type,
            static_cast<unsigned long long>(observation.areaId),
            static_cast<unsigned long long>(diagnostics.playerAreaId),
            observation.worldX, observation.worldY,
            observation.mapX, observation.mapY, decision,
            areaSourceText(observation.areaSource));
    }
    return call;
}

#else
static inline u64 NightLogRedrawDiagnostics(const NightCollectDiagnostics&, size_t, int, int, bool, size_t, size_t) { return 0; }
#endif

// --- Active anchor scan: query book/gear coordinates via 0x1C1A00 -----------
// Instead of passively waiting for the game to call the anchor search
// function (which only fires at save-load time, before the MOD is loaded),
// we actively call 0x1C1A00 for each pop_lost_book%02d and
// pop_machine_part%02d anchor name during the map redraw.  This populates
// g_nightAnchorCache with world coordinates that the existing injection
// mechanism converts to map stamps.

// Build the collected-item set by iterating the save data gimmick list
// and testing each entry's flag bit in the global flag bitmap.  Only
// books appear in this list (machine parts are NOT in the save data
// gimmick list).  Collected books are recorded in g_nightBookCollected
// so NightScanAnchors can skip their anchors.  Machine part collected
// state is probed separately via g_nightGearCollected using the
// machine-part flag index base.
//
// v1.0.20: placed-tonight filtering RESTORED.  capstone disassembly of
// 0x265570 proved flagIdx2 IS the placement flag (not a batch/area ID as
// previously thought).  v1.0.18's removal was based on incomplete log
// analysis: collected books DO have placed=1 (placed in a previous night),
// but uncollected books with placed=0 were never placed tonight.
static void NightBuildCollectedSet() {
    memset(g_nightBookCollected, 0, sizeof(g_nightBookCollected));
    memset(g_nightGearCollected, 0, sizeof(g_nightGearCollected));
    memset(g_nightBookPlaced, 0, sizeof(g_nightBookPlaced));
    memset(g_nightGearPlaced, 0, sizeof(g_nightGearPlaced));
    if (!g_nightSaveDataAccessor) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(
        GetModuleHandleW(nullptr));

    // Read game DB pointer and call save data accessor.
    if (!NightIsReadable(reinterpret_cast<void*>(
            base + RVA_NIGHT_GAME_DB_PTR), sizeof(void*)))
        return;
    void* gameDB = *reinterpret_cast<void**>(
        base + RVA_NIGHT_GAME_DB_PTR);
    if (!gameDB) return;
    void* saveData = nullptr;
    __try {
        saveData = g_nightSaveDataAccessor(gameDB);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!saveData) return;

    // Read global save pointer for flag bitmap base.
    if (!NightIsReadable(reinterpret_cast<void*>(
            base + RVA_NIGHT_SAVE_DATA_PTR), sizeof(void*)))
        return;
    void* savePtr = *reinterpret_cast<void**>(
        base + RVA_NIGHT_SAVE_DATA_PTR);
    if (!savePtr) return;
    void* bitmapBase = nullptr;
    if (!NightReadPointer(savePtr, NIGHT_FLAG_BITMAP_BASE_OFFSET,
                          &bitmapBase) || !bitmapBase)
        return;

    // Test flag bit: qword at [bitmap_base + (flagIdx>>6)*8 + 0x90].
    const auto nightFlagSet = [bitmapBase](uint64_t flagIdx) -> bool {
        if (flagIdx == 0) return false;
        const uint64_t qwordIdx = flagIdx >> 6;
        const uint64_t bitPos = flagIdx & 0x3F;
        const uintptr_t qwordAddr =
            reinterpret_cast<uintptr_t>(bitmapBase) +
            qwordIdx * 8 + NIGHT_FLAG_BITMAP_QWORD_OFFSET;
        if (!NightIsReadable(reinterpret_cast<void*>(qwordAddr),
                sizeof(uint64_t)))
            return false;
        const uint64_t qword = *reinterpret_cast<uint64_t*>(qwordAddr);
        return ((qword >> bitPos) & 1) != 0;
    };

    // --- Book collected set (from save data gimmick list) ---
    // Read gimmick list [saveData+0x98 .. saveData+0xA0].
    void** listBegin = nullptr;
    void** listEnd = nullptr;
    if (!NightReadPointer(saveData, NIGHT_GIMMICK_LIST_BEGIN_OFFSET,
                          reinterpret_cast<void**>(&listBegin)) ||
        !NightReadPointer(saveData, NIGHT_GIMMICK_LIST_END_OFFSET,
                          reinterpret_cast<void**>(&listEnd)))
        return;
    if (!listBegin || !listEnd || listEnd < listBegin) return;
    const size_t entryCount = static_cast<size_t>(
        (reinterpret_cast<uintptr_t>(listEnd) -
         reinterpret_cast<uintptr_t>(listBegin)) / sizeof(void*));
    if (entryCount > 4096) return;  // sanity limit

    size_t bookCollectedCount = 0;
    size_t bookPlacedCount = 0;
    // v1.0.16: one-shot diagnostic — dump all gimmick entries.
    // v1.0.20: also dump flagIdx2 bit state (placed flag).
    static bool s_dumpedEntries = false;
    for (size_t i = 0; i < entryCount; ++i) {
        void* entry = nullptr;
        if (!NightReadPointer(listBegin + i, 0, &entry) || !entry)
            continue;
        void* subObj = nullptr;
        if (!NightReadPointer(entry, 0, &subObj) || !subObj)
            continue;
        if (!NightIsReadable(subObj,
                NIGHT_GIMMICK_ITEM_NUMBER_OFFSET + sizeof(int32_t)))
            continue;
        const uint64_t flagIdx1 = *reinterpret_cast<uint64_t*>(
            reinterpret_cast<uintptr_t>(subObj) +
            NIGHT_GIMMICK_FLAG_IDX1_OFFSET);
        const int32_t itemNum = *reinterpret_cast<int32_t*>(
            reinterpret_cast<uintptr_t>(subObj) +
            NIGHT_GIMMICK_ITEM_NUMBER_OFFSET);
        const uint64_t flagIdx2 = *reinterpret_cast<uint64_t*>(
            reinterpret_cast<uintptr_t>(subObj) +
            NIGHT_GIMMICK_FLAG_IDX2_OFFSET);
        // v1.0.29: 诊断——在 itemNum 过滤之前检测齿轮条目
        // 齿轮的 flagIdx1 应在 24100-24149 范围，或 itemNum >= 63
        if (!s_dumpedEntries && (flagIdx1 >= 24100 || itemNum >= 63)) {
            const bool collected = nightFlagSet(flagIdx1);
            const bool placed = (flagIdx2 == 0) || nightFlagSet(flagIdx2);
            Log("[NightMapMarkers][diag] NON-BOOK entry[%zu] itemNum=%d "
                "flagIdx1=%llu flagIdx2=%llu collected=%d placed=%d "
                "f2bit=%d\n",
                i, itemNum,
                static_cast<unsigned long long>(flagIdx1),
                static_cast<unsigned long long>(flagIdx2),
                static_cast<int>(collected),
                static_cast<int>(placed),
                static_cast<int>(flagIdx2 != 0 ?
                    (nightFlagSet(flagIdx2) ? 1 : 0) : -1));
        }
        if (itemNum < 0 ||
            itemNum + 1 >= static_cast<int32_t>(NIGHT_COLLECTED_MAX))
            continue;
        const bool collected = nightFlagSet(flagIdx1);
        // v1.0.20: flagIdx2 is the placement flag (proven by capstone
        // disasm of 0x265570).  flagIdx2==0 means unconditionally placed.
        // flagIdx2!=0 means bit must be SET for tonight's placement.
        const bool placed = (flagIdx2 == 0) || nightFlagSet(flagIdx2);
        if (!s_dumpedEntries) {
            Log("[NightMapMarkers][diag] entry[%zu] itemNum=%d "
                "flagIdx1=%llu flagIdx2=%llu collected=%d placed=%d "
                "f2bit=%d\n",
                i, itemNum,
                static_cast<unsigned long long>(flagIdx1),
                static_cast<unsigned long long>(flagIdx2),
                static_cast<int>(collected),
                static_cast<int>(placed),
                static_cast<int>(flagIdx2 != 0 ?
                    (nightFlagSet(flagIdx2) ? 1 : 0) : -1));
        }
        if (flagIdx1 == 0)
            continue;
        // v1.0.29: 诊断——检测齿轮是否在存档列表中
        // 如果 flagIdx1 在 24100-24149 范围，说明齿轮条目存在，可读 flagIdx2
        if (flagIdx1 >= 24100 && flagIdx1 < 24150) {
            Log("[NightMapMarkers][diag] GEAR entry found! itemNum=%d "
                "flagIdx1=%llu flagIdx2=%llu placed=%d f2bit=%d\n",
                itemNum,
                static_cast<unsigned long long>(flagIdx1),
                static_cast<unsigned long long>(flagIdx2),
                static_cast<int>(placed),
                static_cast<int>(flagIdx2 != 0 ?
                    (nightFlagSet(flagIdx2) ? 1 : 0) : -1));
        }
        if (collected) {
            g_nightBookCollected[itemNum + 1] = true;
            ++bookCollectedCount;
        }
        // v1.0.20: record placed-tonight state from flagIdx2.
        if (placed) {
            g_nightBookPlaced[itemNum + 1] = true;
            ++bookPlacedCount;
        }
    }
    s_dumpedEntries = true;

    // --- Machine part collected set ---
    // Machine parts use flag indices starting at 24100 (0-based itemNum 0
    // -> flagIdx 24100, itemNum 1 -> 24101, ... itemNum 49 -> 24149).
    // This base is derived from the book pattern (23100 + itemNum) shifted
    // by 1000 for the machine part category.
    static constexpr uint64_t GEAR_FLAGIDX_BASE = 24100;
        // v1.0.20: machine part placed set — gear flagIdx2 is at subObj+0x278,
        // same offset as books.  But gears are NOT in the save gimmick list,
        // so we cannot iterate entries.  Instead we probe flagIdx2 directly.
        // For now, mark all uncollected gears as placed (conservative) until
        // we can identify the correct gear flagIdx2 base.  The 0x1C1A00 found
        // flag will still filter out non-existent anchors on the current map.
        // TODO: find gear flagIdx2 base (book is 23100+itemNum, collected is
        // 24100+itemNum, placement flag may be a third base or same as collected).
    size_t gearCollectedCount = 0;
    size_t gearPlacedCount = 0;
    for (int g = 0; g < 50; ++g) {
        if (nightFlagSet(GEAR_FLAGIDX_BASE + static_cast<uint64_t>(g))) {
            g_nightGearCollected[g + 1] = true;
            ++gearCollectedCount;
        }
        // v1.0.20: conservative — mark all uncollected gears as placed.
        // The 0x1C1A00 found flag is the real filter for gears.
        if (!g_nightGearCollected[g + 1]) {
            g_nightGearPlaced[g + 1] = true;
            ++gearPlacedCount;
        }
    }

    static std::atomic<size_t> s_lastLogged{static_cast<size_t>(-1)};
    const size_t logKey = bookCollectedCount * 10000 +
        bookPlacedCount * 100 + gearCollectedCount * 10 + gearPlacedCount;
    if (s_lastLogged.exchange(logKey,
            std::memory_order_relaxed) != logKey) {
        Log("[NightMapMarkers][filter] entries=%zu "
            "books_collected=%zu books_placed=%zu "
            "gears_collected=%zu gears_placed=%zu\n",
            entryCount, bookCollectedCount, bookPlacedCount,
            gearCollectedCount, gearPlacedCount);
    }
}

// v1.0.29: 精简版活体枚举——只搜 0xE1C168 一个 callback，不调 destroy
// 日志验证 0xE1C168 是唯一命中书/齿轮的 callback，其他 43 个全是空且可能有副作用
// 不调 vtable[4] destroy 避免破坏游戏对象引用计数
static void NightCollectLiveNightItemsSafe(void* /*mapInfo*/) {
    memset(g_nightLiveItems, 0, sizeof(g_nightLiveItems));
    const uintptr_t base = reinterpret_cast<uintptr_t>(
        GetModuleHandleW(nullptr));

    void* root = nullptr;
    void* mapOwner = nullptr;
    void* mapInfoObj = nullptr;
    void* spatialOwner = nullptr;
    if (!NightReadPointer(reinterpret_cast<void*>(base + RVA_GAME_ROOT), 0,
                          &root) || !root ||
        !NightReadPointer(root, NIGHT_ROOT_MAP_OWNER_OFFSET, &mapOwner) ||
        !mapOwner ||
        !NightReadPointer(mapOwner, NIGHT_MAP_INFO_OFFSET, &mapInfoObj) ||
        !mapInfoObj ||
        !NightReadPointer(mapInfoObj, NIGHT_MAP_SPATIAL_OWNER_OFFSET,
                          &spatialOwner) || !spatialOwner) {
        return;
    }
    void* spatialIndex = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(spatialOwner) +
        NIGHT_MAP_SPATIAL_INDEX_OFFSET);
    if (!NightIsReadable(spatialIndex, 0x48)) return;

    const NightRect bounds = *reinterpret_cast<const NightRect*>(
        reinterpret_cast<uintptr_t>(spatialIndex) + 0x10);
    if (!std::isfinite(bounds.minimumX) || !std::isfinite(bounds.minimumY) ||
        !std::isfinite(bounds.maximumX) || !std::isfinite(bounds.maximumY) ||
        bounds.minimumX > bounds.maximumX ||
        bounds.minimumY > bounds.maximumY) {
        return;
    }
    if (!g_nightSpatialSearch || !g_nightRawVectorFree) return;
    if (!g_nightMapAreaResolver || !g_nightLiveAreaResolver) return;

    u64 playerAreaId = 0;
    NightAreaSource playerAreaSource = NightAreaSource::None;
    float playerWorldX = 0.0f;
    float playerWorldY = 0.0f;
    if (!NightResolvePlayerArea(base, root, &playerAreaId,
                                &playerAreaSource,
                                &playerWorldX, &playerWorldY)) {
        return;
    }

    // v1.0.30: 遍历全部 NIGHT_DIAG_CALLBACKS（47 个 callback），不调 destroy。
    // 之前只搜 0xE1C168 一个 callback 只命中 4 书 + 15 齿轮，而锚点缓存有
    // 29 个（9 书 + 20 齿轮），19 个锚点因"活体未找到"被跳过（用户反馈
    // 标识 3/4 没显示）。遍历全部 callback 可覆盖完整活体枚举，不调
    // destroy 避免破坏游戏状态（完整版 44 个 callback + destroy 会致
    // 关闭地图后画面异常）。hit 子集留日志确认。

    size_t bookCount = 0;
    size_t gearCount = 0;
    size_t hitCallbacks = 0;
    size_t areaRejected = 0;
    size_t disabledRejected = 0;

    for (size_t ci = 0; ci < NIGHT_DIAG_CALLBACK_COUNT; ++ci) {
        const uint32_t vtableRva = NIGHT_DIAG_CALLBACKS[ci];

        NightPointerVector results = {};
        u64 filterId = 0;
        NightSearchCallback callback = {};
        *reinterpret_cast<void**>(callback.storage) =
            reinterpret_cast<void*>(base + vtableRva);
        *reinterpret_cast<u64**>(callback.storage + 0x08) = &filterId;
        *reinterpret_cast<NightPointerVector**>(callback.storage + 0x10) =
            &results;
        callback.target = callback.storage;

        __try {
            g_nightSpatialSearch(spatialIndex, &bounds, &callback, -1, -1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            NightReleaseSearchVector(&results);
            continue;
        }

        // 不调 destroy，直接读结果然后释放结果向量

        const uintptr_t rBegin = reinterpret_cast<uintptr_t>(results.begin);
        const uintptr_t rEnd = reinterpret_cast<uintptr_t>(results.end);
        const uintptr_t rCap = reinterpret_cast<uintptr_t>(results.capacity);
        const bool vectorOk = (!rBegin && !rEnd && !rCap) ||
            (rBegin && rEnd >= rBegin && rCap >= rEnd &&
             (rEnd - rBegin) % sizeof(void*) == 0 &&
             (rCap - rBegin) % sizeof(void*) == 0);
        const size_t resultCount = vectorOk && rBegin
            ? static_cast<size_t>((rEnd - rBegin) / sizeof(void*)) : 0;

        bool hitThis = false;
        for (size_t oi = 0; oi < resultCount; ++oi) {
            void* s = results.begin[oi];
            if (!s || !NightIsReadable(s, sizeof(void*))) continue;

            u64 bid = 0;
            void* holder = nullptr;
            if (NightReadPointer(s, 0x240, &holder) && holder) {
                void* data = nullptr;
                if (NightReadPointer(holder, 0, &data) && data &&
                    NightIsReadable(data, sizeof(u64))) {
                    bid = *reinterpret_cast<const u64*>(data);
                }
            }

            size_t slot = static_cast<size_t>(-1);
            if (bid >= NIGHT_BOOK_BASEID_FIRST &&
                bid < NIGHT_BOOK_BASEID_FIRST + 62) {
                slot = static_cast<size_t>(bid - NIGHT_BOOK_BASEID_FIRST);
            } else if (bid >= NIGHT_GEAR_BASEID_FIRST &&
                       bid < NIGHT_GEAR_BASEID_FIRST + 50) {
                slot = 62 + static_cast<size_t>(bid - NIGHT_GEAR_BASEID_FIRST);
            }
            if (slot >= NIGHT_ANCHOR_TOTAL || g_nightLiveItems[slot].found)
                continue;

            // enabled 检查
            if (NightIsReadable(reinterpret_cast<void*>(
                    reinterpret_cast<uintptr_t>(s) +
                    NIGHT_CREATURE_ENABLED_OFFSET), 1) &&
                *reinterpret_cast<const unsigned char*>(
                    reinterpret_cast<uintptr_t>(s) +
                    NIGHT_CREATURE_ENABLED_OFFSET) == 0) {
                ++disabledRejected;
                continue;
            }

            float pos[4] = {0, 0, 0, 0};
            if (NightIsReadable(reinterpret_cast<void*>(
                    reinterpret_cast<uintptr_t>(s) +
                    NIGHT_CREATURE_POSITION_OFFSET), sizeof(float) * 4)) {
                const float* p = reinterpret_cast<const float*>(
                    reinterpret_cast<uintptr_t>(s) +
                    NIGHT_CREATURE_POSITION_OFFSET);
                pos[0] = p[0]; pos[1] = p[1];
                pos[2] = p[2]; pos[3] = p[3];
            }
            if (!std::isfinite(pos[0]) || !std::isfinite(pos[1])) continue;

            // 同区域检查
            u64 itemAreaId = 0;
            NightAreaSource itemAreaSource = NightAreaSource::None;
            if (!NightResolveAreaId(s, pos, &itemAreaId, &itemAreaSource) ||
                itemAreaId != playerAreaId) {
                ++areaRejected;
                continue;
            }

            g_nightLiveItems[slot].found = true;
            memcpy(g_nightLiveItems[slot].worldPos, pos, sizeof(float) * 4);
            if (slot < 62) ++bookCount;
            else ++gearCount;
            hitThis = true;
        }

        NightReleaseSearchVector(&results);
        if (hitThis) {
            ++hitCallbacks;
            Log("[NightMapMarkers][filter] live hit cb=0x%X (books=%zu "
                "gears=%zu)\n", vtableRva, bookCount, gearCount);
        }
    }

    Log("[NightMapMarkers][filter] live_safe books=%zu gears=%zu "
        "callbacks=%zu hitCallbacks=%zu areaRejected=%zu "
        "disabledRejected=%zu\n",
        bookCount, gearCount, NIGHT_DIAG_CALLBACK_COUNT, hitCallbacks,
        areaRejected, disabledRejected);
}

// v1.0.10: enumerate live books/gears via the SHARED SPATIAL INDEX,
// probing every candidate callback vtable in NIGHT_DIAG_CALLBACKS.
//
// History:
//   v1.0.8 used the single "all objects" callback 0xE1C168 -> returned 0
//   (it only enumerates creature entries, not gimmicks).
//   v1.0.9 used the g_gimmickMgr hash table -> returned 0 (that table only
//   holds static decorative furniture: keys 6067..6597, all enabled=0).
//   The v1.0.7 F7 brute-force proved books/gears DO live in the shared
//   spatial index and are reachable through a subset of the candidate
//   callback vtables, so this is the source of truth.
//
// Every found callback that returns at least one book/gear is recorded in
// s_liveHitCallbacks; once any hits exist, later scans only re-probe the
// hit set instead of walking all 47 (which costs ~ms per search).
static void NightCollectLiveNightItems(void* /*mapInfo*/) {
    memset(g_nightLiveItems, 0, sizeof(g_nightLiveItems));
    const uintptr_t base = reinterpret_cast<uintptr_t>(
        GetModuleHandleW(nullptr));

    void* root = nullptr;
    void* mapOwner = nullptr;
    void* mapInfoObj = nullptr;
    void* spatialOwner = nullptr;
    if (!NightReadPointer(reinterpret_cast<void*>(base + RVA_GAME_ROOT), 0,
                          &root) || !root ||
        !NightReadPointer(root, NIGHT_ROOT_MAP_OWNER_OFFSET, &mapOwner) ||
        !mapOwner ||
        !NightReadPointer(mapOwner, NIGHT_MAP_INFO_OFFSET, &mapInfoObj) ||
        !mapInfoObj ||
        !NightReadPointer(mapInfoObj, NIGHT_MAP_SPATIAL_OWNER_OFFSET,
                          &spatialOwner) || !spatialOwner) {
        Log("[NightMapMarkers][filter] live spatial chain unavailable\n");
        return;
    }
    void* spatialIndex = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(spatialOwner) +
        NIGHT_MAP_SPATIAL_INDEX_OFFSET);
    if (!NightIsReadable(spatialIndex, 0x48)) {
        Log("[NightMapMarkers][filter] live spatial index unreadable\n");
        return;
    }
    const NightRect bounds = *reinterpret_cast<const NightRect*>(
        reinterpret_cast<uintptr_t>(spatialIndex) + 0x10);
    if (!std::isfinite(bounds.minimumX) || !std::isfinite(bounds.minimumY) ||
        !std::isfinite(bounds.maximumX) || !std::isfinite(bounds.maximumY) ||
        bounds.minimumX > bounds.maximumX ||
        bounds.minimumY > bounds.maximumY) {
        Log("[NightMapMarkers][filter] live spatial bounds invalid\n");
        return;
    }
    if (!g_nightSpatialSearch || !g_nightRawVectorFree) {
        Log("[NightMapMarkers][filter] native search unavailable\n");
        return;
    }
    // v1.0.12: require area resolvers for same-area filtering.
    if (!g_nightMapAreaResolver || !g_nightLiveAreaResolver) {
        Log("[NightMapMarkers][filter] area resolvers unavailable\n");
        return;
    }
    u64 playerAreaId = 0;
    NightAreaSource playerAreaSource = NightAreaSource::None;
    float playerWorldX = 0.0f;
    float playerWorldY = 0.0f;
    if (!NightResolvePlayerArea(base, root, &playerAreaId,
                                &playerAreaSource,
                                &playerWorldX, &playerWorldY)) {
        Log("[NightMapMarkers][filter] live player area unavailable\n");
        return;
    }

    // v1.0.29: 移除 s_hitCallbacks 永久缓存——每次都搜索所有 callback
    // 之前未命中的 callback 会被永久跳过，导致漏检今晚新生成的书/齿轮
    static size_t s_hitCount = 0;  // 保留用于首次诊断日志

    size_t searchedCallbacks = 0;
    size_t bookCount = 0;
    size_t gearCount = 0;
    size_t areaRejected = 0;
    size_t disabledRejected = 0;

    for (size_t ci = 0; ci < NIGHT_DIAG_CALLBACK_COUNT; ++ci) {
        ++searchedCallbacks;
        const uint32_t vtableRva = NIGHT_DIAG_CALLBACKS[ci];

        NightPointerVector results = {};
        u64 filterId = 0;
        NightSearchCallback callback = {};
        *reinterpret_cast<void**>(callback.storage) =
            reinterpret_cast<void*>(base + vtableRva);
        *reinterpret_cast<u64**>(callback.storage + 0x08) = &filterId;
        *reinterpret_cast<NightPointerVector**>(callback.storage + 0x10) =
            &results;
        callback.target = callback.storage;

        bool searched = false;
        __try {
            searched = g_nightSpatialSearch(spatialIndex, &bounds,
                                            &callback, -1, -1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            searched = false;
        }

        if (callback.target) {
            void** vtable = NightIsReadable(callback.target, sizeof(void*))
                ? *reinterpret_cast<void***>(callback.target) : nullptr;
            if (vtable && NightIsReadable(vtable, sizeof(void*) * 5)) {
                using DestroyFunction = void (__fastcall *)(void*, bool);
                __try {
                    reinterpret_cast<DestroyFunction>(vtable[4])(
                        callback.target,
                        callback.target != callback.storage);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
            callback.target = nullptr;
        }

        const uintptr_t rBegin = reinterpret_cast<uintptr_t>(results.begin);
        const uintptr_t rEnd = reinterpret_cast<uintptr_t>(results.end);
        const uintptr_t rCap = reinterpret_cast<uintptr_t>(results.capacity);
        const bool vectorOk = (!rBegin && !rEnd && !rCap) ||
            (rBegin && rEnd >= rBegin && rCap >= rEnd &&
             (rEnd - rBegin) % sizeof(void*) == 0 &&
             (rCap - rBegin) % sizeof(void*) == 0);
        const size_t resultCount = vectorOk && rBegin
            ? static_cast<size_t>((rEnd - rBegin) / sizeof(void*)) : 0;

        bool hitThis = false;
        // v1.0.10: first-scan diagnostic — log each callback's result
        // count and sample baseIDs (only when no hits cached yet).
        const bool firstScan = (s_hitCount == 0);
        u64 sampleBids[3] = {0, 0, 0};
        size_t sampleCount = 0;
        if (vectorOk && resultCount) {
            for (size_t oi = 0; oi < resultCount; ++oi) {
                void* s = results.begin[oi];
                if (!s || !NightIsReadable(s, sizeof(void*))) continue;
                // baseID reading: status+0x240 -> holder -> holder[0]=data
                //                         -> data[0]=u64 baseID  (3 derefs)
                u64 bid = 0;
                void* holder = nullptr;
                if (NightReadPointer(s, 0x240, &holder) && holder) {
                    void* data = nullptr;
                    if (NightReadPointer(holder, 0, &data) && data &&
                        NightIsReadable(data, sizeof(u64))) {
                        bid = *reinterpret_cast<const u64*>(data);
                    }
                }
                if (firstScan && sampleCount < 3 && bid != 0) {
                    sampleBids[sampleCount] = bid;
                    ++sampleCount;
                }
                size_t slot = static_cast<size_t>(-1);
                if (bid >= NIGHT_BOOK_BASEID_FIRST &&
                    bid < NIGHT_BOOK_BASEID_FIRST + 62) {
                    slot = static_cast<size_t>(bid - NIGHT_BOOK_BASEID_FIRST);
                } else if (bid >= NIGHT_GEAR_BASEID_FIRST &&
                           bid < NIGHT_GEAR_BASEID_FIRST + 50) {
                    slot = 62 + static_cast<size_t>(
                        bid - NIGHT_GEAR_BASEID_FIRST);
                }
                if (slot < NIGHT_ANCHOR_TOTAL &&
                    !g_nightLiveItems[slot].found) {
                    // v1.0.13: reject objects whose enabled flag is 0 --
                    // the spatial index contains ALL defined book/gear
                    // templates, but only enabled ones are actually placed
                    // tonight.  Without this check every defined anchor
                    // is marked and most are empty when walked to.
                    if (NightIsReadable(reinterpret_cast<void*>(
                            reinterpret_cast<uintptr_t>(s) +
                            NIGHT_CREATURE_ENABLED_OFFSET), 1) &&
                        *reinterpret_cast<const unsigned char*>(
                            reinterpret_cast<uintptr_t>(s) +
                            NIGHT_CREATURE_ENABLED_OFFSET) == 0) {
                        ++disabledRejected;
                        continue;
                    }
                    float pos[4] = {0, 0, 0, 0};
                    if (NightIsReadable(
                            reinterpret_cast<void*>(
                                reinterpret_cast<uintptr_t>(s) +
                                NIGHT_CREATURE_POSITION_OFFSET),
                            sizeof(float) * 4)) {
                        const float* p = reinterpret_cast<const float*>(
                            reinterpret_cast<uintptr_t>(s) +
                            NIGHT_CREATURE_POSITION_OFFSET);
                        pos[0] = p[0]; pos[1] = p[1];
                        pos[2] = p[2]; pos[3] = p[3];
                    }
                    if (std::isfinite(pos[0]) && std::isfinite(pos[1])) {
                        // v1.0.12: only accept items in the same area as
                        // the player -- cross-area items are unreachable.
                        u64 itemAreaId = 0;
                        NightAreaSource itemAreaSource =
                            NightAreaSource::None;
                        if (!NightResolveAreaId(s, pos, &itemAreaId,
                                               &itemAreaSource) ||
                            itemAreaId != playerAreaId) {
                            ++areaRejected;
                            continue;
                        }
                        // v1.0.14: one-shot diagnostic — dump key fields
                        // for every accepted book/gear to understand why
                        // they appear on the map but are empty when walked to.
                        {
                            static bool s_dumped = false;
                            if (!s_dumped) {
                                void* liveObj = nullptr;
                                void* liveVal = nullptr;
                                NightReadPointer(s, 0x150, &liveObj);
                                NightReadPointer(s, 0x158, &liveVal);
                                uint8_t enByte = 0xFF;
                                if (NightIsReadable(reinterpret_cast<void*>(
                                        reinterpret_cast<uintptr_t>(s)+0x130), 1))
                                    enByte = *reinterpret_cast<const uint8_t*>(
                                        reinterpret_cast<uintptr_t>(s)+0x130);
                                Log("[NightMapMarkers][diag] ACCEPT slot=%zu "
                                    "bid=%llu en=%u pos=(%.1f,%.1f,%.1f) "
                                    "liveObj=%p liveVal=%p s=%p\n",
                                    slot,
                                    static_cast<unsigned long long>(bid),
                                    static_cast<unsigned>(enByte),
                                    pos[0], pos[1], pos[2],
                                    liveObj, liveVal, s);
                            }
                        }
                        g_nightLiveItems[slot].found = true;
                        memcpy(g_nightLiveItems[slot].worldPos, pos,
                               sizeof(float) * 4);
                        if (slot < 62) ++bookCount;
                        else ++gearCount;
                        hitThis = true;
                    }
                }
            }
        }

        // v1.0.10: first-scan per-callback diagnostic
        if (firstScan && hitThis) {
            Log("[NightMapMarkers][filter] live hit cb=0x%X "
                "(books=%zu gears=%zu)\n",
                vtableRva, bookCount, gearCount);
        }

        NightReleaseSearchVector(&results);
    }

    Log("[NightMapMarkers][filter] live_items books=%zu gears=%zu "
        "callbacks=%zu/%zu hitCbs=%zu areaRejected=%zu disabledRejected=%zu\n",
        bookCount, gearCount, searchedCallbacks,
        NIGHT_DIAG_CALLBACK_COUNT, s_hitCount, areaRejected,
        disabledRejected);
}

static void NightScanAnchors(void* mapInfo, u64 playerAreaId) {
    if (!mapInfo) return;
    NightExactAnchorSearchFunc func = g_originalNightAnchorSearch
        ? g_originalNightAnchorSearch
        : g_nightAnchorSearchDirect;
    if (!func || !g_nightMapHashLookup) return;

    // Throttle: scan on first call and every NIGHT_ANCHOR_SCAN_INTERVAL
    // calls after that.  Once progressive scan completes the cache is
    // populated and further calls skip until the interval expires.
    // v1.0.25: 地图切换（areaId 变化）时强制重扫——避免残留上一张地图
    // 的锚点缓存导致"少标"（用户反馈山顶少一个零件和一个书）。
    // 用 areaId 作为地图标识：玩家跨区域时 areaId 变化即触发重扫。
    const bool mapChanged = (playerAreaId != g_nightAnchorScannedAreaId);
    if (mapChanged) {
        // 地图切换：强制清空旧缓存并重新扫描
        g_nightAnchorScanCounter.store(0, std::memory_order_relaxed);
        for (size_t i = 0; i < NIGHT_ANCHOR_CACHE_CAPACITY; ++i)
            g_nightAnchorCache[i].valid = false;
        g_nightAnchorCacheCount.store(0, std::memory_order_release);
        g_nightProgressive.active = false;
        g_nightProgressive.nextIndex = 0;
        g_nightProgressive.cachedCount = 0;
        g_nightAnchorScannedAreaId = playerAreaId;
        Log("[NightMapMarkers][anchor] area changed; cache cleared for re-scan "
            "(areaId=%llu)\n",
            static_cast<unsigned long long>(playerAreaId));
    }

    const u64 callCount = g_nightAnchorScanCounter.fetch_add(
        1, std::memory_order_relaxed);
    const size_t prevCount = g_nightAnchorCacheCount.load(
        std::memory_order_relaxed);
    const bool progressiveActive = g_nightProgressive.active;
    if (!progressiveActive && prevCount > 0 &&
        callCount % NIGHT_ANCHOR_SCAN_INTERVAL != 0)
        return;

    // If a progressive scan is not active and we need to (re)scan, start one.
    if (!progressiveActive) {
        // Resolve the CCom_Map object via the same chain the game uses:
        void* spatialOwner = nullptr;
        if (!NightReadPointer(mapInfo, NIGHT_MAP_SPATIAL_OWNER_OFFSET,
                              &spatialOwner) || !spatialOwner) {
            Log("[NightMapMarkers][anchor] spatialOwner unavailable; scan skipped\n");
            return;
        }
        const uint64_t areaId = static_cast<uint64_t>(playerAreaId);
        void* entry = nullptr;
        __try {
            entry = g_nightMapHashLookup(spatialOwner, areaId);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[NightMapMarkers][anchor] hash lookup SEH; areaId=%llu\n",
                static_cast<unsigned long long>(areaId));
            return;
        }
        if (!entry) {
            Log("[NightMapMarkers][anchor] hash lookup returned null; "
                "areaId=%llu spatialOwner=%p\n",
                static_cast<unsigned long long>(areaId), spatialOwner);
            return;
        }
        void* mapObj = nullptr;
        if (!NightReadPointer(entry, 0, &mapObj) || !mapObj) {
            Log("[NightMapMarkers][anchor] mapObj unreadable from entry=%p\n",
                entry);
            return;
        }

        // v1.0.29: 恢复 collected/placed 过滤——只显示未收集 && 今晚放置的锚点
        NightBuildCollectedSet();

        // Start a new progressive scan
        g_nightProgressive.mapObj = mapObj;
        g_nightProgressive.nextIndex = 0;
        g_nightProgressive.cachedCount = 0;
        g_nightProgressive.active = true;
        // Invalidate all cache entries so old markers don't linger
        for (size_t i = 0; i < NIGHT_ANCHOR_CACHE_CAPACITY; ++i)
            g_nightAnchorCache[i].valid = false;
        g_nightAnchorCacheCount.store(0, std::memory_order_release);
    }

    // Process one batch of anchors this frame
    void* mapObj = g_nightProgressive.mapObj;
    size_t idx = g_nightProgressive.nextIndex;
    size_t cached = g_nightProgressive.cachedCount;
    char anchorName[32];
    size_t processed = 0;

    while (idx < NIGHT_ANCHOR_TOTAL &&
           processed < NIGHT_ANCHOR_BATCH_SIZE &&
           cached < NIGHT_ANCHOR_CACHE_CAPACITY) {
        int itemNum = 0;
        if (idx < 62) {
            itemNum = static_cast<int>(idx + 1);
            _snprintf_s(anchorName, sizeof(anchorName), _TRUNCATE,
                         "pop_lost_book%02d", itemNum);
        } else {
            itemNum = static_cast<int>(idx - 62 + 1);
            _snprintf_s(anchorName, sizeof(anchorName), _TRUNCATE,
                         "pop_machine_part%02d", itemNum);
        }

        // v1.0.34: 只过滤已收集——书/齿轮全部显示（不区分今晚是否放置）。
        // placed 标志不可靠（齿轮无 placed 数据，书的 flagIdx2 是模板级
        // 标志），用户选择"全部显示"方案：0x1C1A00 found 锚点全标。
        bool isCollected = false;
        if (idx < 62) {
            isCollected = g_nightBookCollected[itemNum];
        } else {
            isCollected = g_nightGearCollected[itemNum];
        }
        ++idx;
        if (isCollected) continue;

        // v1.0.15: query 0x1C1A00 for the anchor's world coordinates --
        // the same function the game's own placement code (0x2657F0) uses.
        // outBuf layout: [0x00..0x0F]=float4 xyzw, [0x20]=found flag.
        alignas(16) unsigned char outBuf[0x28] = {};
        __try {
            func(mapObj, outBuf, anchorName);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (outBuf[0x20] == 0) continue;  // anchor not found on this map
        const float* coords = reinterpret_cast<const float*>(outBuf);
        if (!std::isfinite(coords[0]) || !std::isfinite(coords[1]))
            continue;

        strncpy_s(g_nightAnchorCache[cached].name,
                   sizeof(g_nightAnchorCache[cached].name),
                   anchorName, _TRUNCATE);
        g_nightAnchorCache[cached].worldPos[0] = coords[0];
        g_nightAnchorCache[cached].worldPos[1] = coords[1];
        g_nightAnchorCache[cached].worldPos[2] = coords[2];
        g_nightAnchorCache[cached].worldPos[3] = coords[3];
        g_nightAnchorCache[cached].valid = true;
        ++cached;
        ++processed;
    }

    g_nightProgressive.nextIndex = idx;
    g_nightProgressive.cachedCount = cached;

    // Publish partial results so markers appear progressively
    g_nightAnchorCacheCount.store(cached, std::memory_order_release);

    // If all anchors have been processed, finalize the scan
    if (idx >= NIGHT_ANCHOR_TOTAL) {
        for (size_t i = cached; i < NIGHT_ANCHOR_CACHE_CAPACITY; ++i)
            g_nightAnchorCache[i].valid = false;
        g_nightAnchorCacheCount.store(cached, std::memory_order_release);
        g_nightProgressive.active = false;

        static std::atomic<size_t> s_lastLogged{static_cast<size_t>(-1)};
        if (s_lastLogged.exchange(cached, std::memory_order_relaxed) != cached) {
            Log("[NightMapMarkers][anchor] progressive_scan_complete: "
                "%zu anchors cached\n", cached);
            // v1.0.17: dump each cached anchor's name so we can see whether
            // the 12 anchors are books, gears, or a mix — the current log
            // only shows cache_count, not the actual names.
            size_t bookCount = 0;
            size_t gearCount = 0;
            for (size_t di = 0; di < cached; ++di) {
                const char* nm = g_nightAnchorCache[di].name;
                if (strncmp(nm, "pop_lost_book", 13) == 0)
                    ++bookCount;
                else if (strncmp(nm, "pop_machine_part", 16) == 0)
                    ++gearCount;
                Log("[NightMapMarkers][anchor] cached[%zu]=%s "
                    "world=(%.1f,%.1f)\n",
                    di, nm,
                    g_nightAnchorCache[di].worldPos[0],
                    g_nightAnchorCache[di].worldPos[1]);
            }
            Log("[NightMapMarkers][anchor] cached_totals books=%zu gears=%zu\n",
                bookCount, gearCount);
        }
    }
}

// v1.0.36 fix: hour = ((rawSecond % 86400) / 3600 + 7) % 24（+7 为显示偏移，
// raw 编码 0 点 = 游戏显示 7 点，与 TimeFreeze/SelfServiceStore 同源已实测）。
// 原实现漏 +7：显示 7~17 点误判为夜（白天执行夜间扫描+标记注入）、
// 显示 18~24 点误判为白天（幽灵鼠/宝箱标记在 18~24 点失效）。
// 白天 = 06:00~18:00。
static bool NightIsDaytime(uintptr_t exeBase) {
    // v1.0.49-diag 修复：rawSecond 读取链缺 +0x208 间接层（AutoFish 同源链为
    // root(0x10FCBB0) -> +SAVE_DATA_OFFSET 0x208 -> saveData -> +0x3270）。
    // 旧链把 root slot 指针直接当 saveData 用，恒读 0 -> hour=7 误判白天 ->
    // 全天静默过滤（2026-10-06 三针日志实锤 daytime_filtered raw_second=0 hour=7）。
    void* root = *reinterpret_cast<void**>(exeBase + RVA_NIGHT_SAVE_DATA_PTR);
    if (!root) return false;  // 读不到就不过滤（放行夜间搜索）
    void* savePtr = nullptr;
    int64_t rawSecond = 0;
    __try {
        savePtr = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(root) + 0x208);  // SAVE_DATA_OFFSET
        if (!savePtr) return false;
        rawSecond = *reinterpret_cast<int64_t*>(
            reinterpret_cast<unsigned char*>(savePtr) + 0x3270);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;  // SEH 异常时不过滤
    }
    if (rawSecond <= 0) return false;  // v1.0.49-diag: 0 值也 fail-open（旧版 0 会误判白天）
    int hour = static_cast<int>((((rawSecond % 86400) / 3600) + 7) % 24);
    return (hour >= 6 && hour < 18);
}

#ifdef MONSTERMARK_LOGGING
// v1.0.48-diag: 诊断读取 rawSecond/hour——独立小函数以允许 SEH
//（detour 主函数含需栈展开的 C++ 对象，不能直接 __try，参照 NightIsDaytime 先例）
static void NightDiagLogDaytimeFilter(u64 call) {
    int64_t rawDiag = 0;
    int hourDiag = -1;
    __try {
        const uintptr_t exeBase =
            reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        void* rootDiag = *reinterpret_cast<void**>(
            exeBase + RVA_NIGHT_SAVE_DATA_PTR);
        if (rootDiag) {
            // v1.0.49-diag: 与 NightIsDaytime 同步修链（+0x208 间接层）
            void* savePtrDiag = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(rootDiag) + 0x208);
            if (savePtrDiag) {
                rawDiag = *reinterpret_cast<int64_t*>(
                    reinterpret_cast<unsigned char*>(savePtrDiag) + 0x3270);
                hourDiag = static_cast<int>(
                    (((rawDiag % 86400) / 3600) + 7) % 24);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hourDiag = -2;
    }
    Log("[NightMapMarkers][diag] daytime_filtered call=%llu "
        "raw_second=%lld hour=%d\n",
        static_cast<unsigned long long>(call),
        static_cast<long long>(rawDiag), hourDiag);
}
#endif  // MONSTERMARK_LOGGING

static void __fastcall NightMapRedrawDetour(void* mapTask) {
    if (!g_originalNightMapRedraw) return;
#ifdef MONSTERMARK_LOGGING
    // v1.0.48-diag: detour 触发诊断——前 3 次必打 + 之后每 600 次打一条。
    // 背景（2026-10-06 用户实测夜间开地图无标记且运行期零日志）：前 12 次去重
    // 逻辑意味着零日志=detour 未被游戏调用 / 或被下方静默早退拦截，三针定位。
    // v1.0.50 正式版随 MONSTERMARK_LOGGING 关闭（2026-10-06 用户实测通过）。
    {
        static u64 diagEntryCalls = 0;
        static u64 diagEntryLastLogged = 0;
        ++diagEntryCalls;
        if (diagEntryCalls <= 3 ||
            diagEntryCalls - diagEntryLastLogged >= 600) {
            diagEntryLastLogged = diagEntryCalls;
            Log("[NightMapMarkers][diag] detour_entered call=%llu "
                "enabled=%d faulted=%d inside=%d\n",
                static_cast<unsigned long long>(diagEntryCalls),
                g_nightMapMarkersEnabled.load(std::memory_order_relaxed) ? 1 : 0,
                g_nightMapMarkersFaulted.load(std::memory_order_relaxed) ? 1 : 0,
                g_insideNightMapRedraw ? 1 : 0);
        }
    }
#endif
    const DWORD threadId = GetCurrentThreadId();
    DWORD expectedThread = 0;
    if (!g_nightRedrawThreadId.compare_exchange_strong(
            expectedThread, threadId, std::memory_order_acq_rel) &&
        expectedThread != threadId) {
        g_nightMapMarkersFaulted.store(true, std::memory_order_relaxed);
        Log("[NightMapMarkers] redraw thread changed expected=%lu actual=%lu; "
            "native redraw only and future markers disabled safely\n",
            expectedThread, threadId);
        g_originalNightMapRedraw(mapTask);
        return;
    }
    if (g_insideNightMapRedraw ||
        !g_nightMapMarkersEnabled.load(std::memory_order_relaxed) ||
        g_nightMapMarkersFaulted.load(std::memory_order_relaxed)) {
        g_originalNightMapRedraw(mapTask);
        return;
    }

    // v1.0.29: 昼夜过滤——白天不执行夜间标记空间搜索
    {
        const uintptr_t exeBase =
            reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (NightIsDaytime(exeBase)) {
#ifdef MONSTERMARK_LOGGING
            // v1.0.48-diag: 白天过滤诊断——实锤昼夜判定值（防 rawSecond 偏移漂移
            // 读到垃圾正数被误判白天而静默拦截；前 3 次+每 300 次打一条）
            {
                static u64 diagDayFiltered = 0;
                static u64 diagDayLastLogged = 0;
                ++diagDayFiltered;
                if (diagDayFiltered <= 3 ||
                    diagDayFiltered - diagDayLastLogged >= 300) {
                    diagDayLastLogged = diagDayFiltered;
                    NightDiagLogDaytimeFilter(diagDayFiltered);
                }
            }
#endif
            g_originalNightMapRedraw(mapTask);
            return;
        }
    }

    g_insideNightMapRedraw = true;
    NightMarkerTarget targets[NIGHT_MAX_TARGETS] = {};
    int ghostRatCount = 0;
    int treasureBoxCount = 0;
    NightCollectDiagnostics diagnostics = {};
    LARGE_INTEGER collectStarted = {};
    LARGE_INTEGER collectFinished = {};
    LARGE_INTEGER performanceFrequency = {};
    NightReadableRegionCache readableCache = {};
    QueryPerformanceCounter(&collectStarted);
    size_t targetCount = 0;
    {
        NightReadableRegionCacheScope readableScope(&readableCache);
        targetCount = NightCollectTargets(
            targets, &ghostRatCount, &treasureBoxCount, &diagnostics);
    }
    QueryPerformanceCounter(&collectFinished);
    diagnostics.readableChecks = readableCache.checks;
    diagnostics.readableCacheHits = readableCache.cacheHits;
    diagnostics.readableVirtualQueries = readableCache.virtualQueries;
    diagnostics.readableRegions = readableCache.count;
    if (QueryPerformanceFrequency(&performanceFrequency) &&
        performanceFrequency.QuadPart > 0 &&
        collectFinished.QuadPart >= collectStarted.QuadPart) {
        diagnostics.elapsedMicroseconds = static_cast<u64>(
            (collectFinished.QuadPart - collectStarted.QuadPart) * 1000000ULL /
            performanceFrequency.QuadPart);
        if (readableCache.virtualQueryTicks >= 0) {
            diagnostics.readableVirtualQueryMicroseconds = static_cast<u64>(
                readableCache.virtualQueryTicks * 1000000ULL /
                performanceFrequency.QuadPart);
        }
    }
    // --- Active anchor scan: query book/gear coordinates via 0x1C1A00 ---
    // v1.0.9: NightScanAnchors 内 NightCollectLiveNightItems 遍历 g_gimmickMgr
    // 哈希表，纳入可读缓存作用域避免每次 VirtualQuery（日志 vq_us≈39ms 即来源）
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    void* root = nullptr;
    void* mapOwner = nullptr;
    void* mapInfo = nullptr;
    {
        NightReadableRegionCacheScope anchorScope(&readableCache);
        if (NightReadPointer(reinterpret_cast<void*>(base + RVA_GAME_ROOT), 0, &root) &&
            NightReadPointer(root, NIGHT_ROOT_MAP_OWNER_OFFSET, &mapOwner) &&
            NightReadPointer(mapOwner, NIGHT_MAP_INFO_OFFSET, &mapInfo)) {
            // v1.0.35: 停用书/齿轮锚点标记——0x1C1A00 无法区分"锚点有定义"
            // 与"今晚实际生成"，用户实测仍有空标。暂不显示书/齿轮，
            // 只保留幽灵鼠/宝箱标记。待未来逆向"实际生成物列表"后再恢复。
            // NightScanAnchors(mapInfo, diagnostics.playerAreaId);
        }
    }

    const size_t anchorCount = g_nightAnchorCacheCount.load(
        std::memory_order_relaxed);
    if (g_nightMapMarkersFaulted.load(std::memory_order_relaxed) ||
        (targetCount == 0 && anchorCount == 0)) {
        NightLogRedrawDiagnostics(diagnostics, targetCount, ghostRatCount,
                                  treasureBoxCount, false, 0, 0);
        g_originalNightMapRedraw(mapTask);
        g_insideNightMapRedraw = false;
        return;
    }

    void* mainStatus = nullptr;
    if (!root || !NightReadPointer(root, NIGHT_ROOT_MAIN_STATUS_OFFSET, &mainStatus)) {
        NightLogRedrawDiagnostics(diagnostics, targetCount, ghostRatCount,
                                  treasureBoxCount, false, 0, 0);
        g_originalNightMapRedraw(mapTask);
        g_insideNightMapRedraw = false;
        return;
    }
    NightPointerVector* stampVector = reinterpret_cast<NightPointerVector*>(
        reinterpret_cast<uintptr_t>(mainStatus) + NIGHT_STAMP_VECTOR_OFFSET);
    if (!IsReadable(stampVector, sizeof(*stampVector))) {
        NightLogRedrawDiagnostics(diagnostics, targetCount, ghostRatCount,
                                  treasureBoxCount, false, 0, 0);
        Log("[NightMapMarkers] persistent stamp vector is not readable; "
            "native redraw only\n");
        g_originalNightMapRedraw(mapTask);
        g_insideNightMapRedraw = false;
        return;
    }
    const NightPointerVector saved = *stampVector;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(saved.begin);
    const uintptr_t end = reinterpret_cast<uintptr_t>(saved.end);
    const uintptr_t capacity = reinterpret_cast<uintptr_t>(saved.capacity);
    const bool vectorOk = (!begin && !end && !capacity) ||
        (begin && end >= begin && capacity >= end &&
         (end - begin) % sizeof(void*) == 0 &&
         (capacity - begin) % sizeof(void*) == 0);
    const size_t existingCount = vectorOk && begin
        ? static_cast<size_t>((end - begin) / sizeof(void*)) : 0;
    const size_t existingCapacity = vectorOk && begin
        ? static_cast<size_t>((capacity - begin) / sizeof(void*)) : 0;
    if (!vectorOk || existingCount > NIGHT_MAX_EXISTING_STAMPS ||
        (existingCount &&
         !IsReadable(saved.begin, existingCount * sizeof(void*)))) {
        NightLogRedrawDiagnostics(diagnostics, targetCount, ghostRatCount,
                                  treasureBoxCount, vectorOk, existingCount,
                                  existingCapacity);
        Log("[NightMapMarkers] persistent stamp vector rejected existing=%zu "
            "capacity=%zu native_limit=%zu; native redraw only\n",
            existingCount, existingCapacity, NIGHT_MAX_EXISTING_STAMPS);
        g_originalNightMapRedraw(mapTask);
        g_insideNightMapRedraw = false;
        return;
    }
    const u64 loggedRedraw = NightLogRedrawDiagnostics(
        diagnostics, targetCount, ghostRatCount, treasureBoxCount, true,
        existingCount, existingCapacity);

    NightFakeMapStamp fakeStamps[NIGHT_MAX_TARGETS + NIGHT_ANCHOR_CACHE_CAPACITY] = {};
    void* combined[NIGHT_MAX_EXISTING_STAMPS + NIGHT_MAX_TARGETS +
                   NIGHT_ANCHOR_CACHE_CAPACITY] = {};
    if (existingCount) memcpy(combined, saved.begin, existingCount * sizeof(void*));
    for (size_t i = 0; i < targetCount; ++i) {
        fakeStamps[i].vtable =
            reinterpret_cast<void*>(base + RVA_NIGHT_MAP_STAMP_VTABLE);
        fakeStamps[i].references = 1;
        fakeStamps[i].stampType = targets[i].stampType;
        memcpy(fakeStamps[i].position, targets[i].mapPosition,
               sizeof(float) * 4);
        combined[existingCount + i] = &fakeStamps[i];
    }
    // Append anchor-cached book/gear targets (0x1C1A00 coordinates).
    // v1.0.33: 回归锚点缓存绘制。诊断证实活体枚举（0xE1C168）读到的
    // baseID/坐标不是书/齿轮实体——baseID 490400000-003 对应 book01-04，
    // 但 placed 表说今晚生成的是 book06/21/24/...，编号对不上；坐标与
    // 0x1C1A00 锚点偏移几百到几千。0xE1C168 历史注释已警告"对书/齿轮
    // 返回 0"。0x1C1A00 是游戏放置逻辑 0x2657F0 同款查询，坐标可靠。
    // collected/placed 过滤已在 NightScanAnchors 中完成。
    size_t anchorStampCount = 0;
    const size_t anchorCacheCount = g_nightAnchorCacheCount.load(
        std::memory_order_relaxed);
    for (size_t ai = 0; ai < anchorCacheCount; ++ai) {
        if (!g_nightAnchorCache[ai].valid) continue;
        const char* name = g_nightAnchorCache[ai].name;
        int anchorStampType = -1;
        if (strncmp(name, "pop_machine_part", 16) == 0) {
            anchorStampType = NIGHT_STAMP_TYPE_MACHINE_PART;
        } else if (strncmp(name, "pop_lost_book", 13) == 0) {
            anchorStampType = NIGHT_STAMP_TYPE_LOST_BOOK;
        } else {
            continue;
        }
        const float* wp = g_nightAnchorCache[ai].worldPos;
        if (!std::isfinite(wp[0]) || !std::isfinite(wp[1])) continue;
        alignas(16) float worldPos[4] = {};
        alignas(16) float mapPos[4] = {};
        memcpy(worldPos, wp, sizeof(worldPos));
        if (!NightTransformWorldToMap(worldPos, mapPos)) continue;
        if (targetCount + anchorStampCount >=
            NIGHT_MAX_TARGETS + NIGHT_ANCHOR_CACHE_CAPACITY) break;
        const size_t stampIdx = targetCount + anchorStampCount;
        fakeStamps[stampIdx].vtable =
            reinterpret_cast<void*>(base + RVA_NIGHT_MAP_STAMP_VTABLE);
        fakeStamps[stampIdx].references = 1;
        fakeStamps[stampIdx].stampType = anchorStampType;
        memcpy(fakeStamps[stampIdx].position, mapPos, sizeof(float) * 4);
        combined[existingCount + stampIdx] = &fakeStamps[stampIdx];
        ++anchorStampCount;
    }
    if (anchorStampCount > 0) {
        Log("[NightMapMarkers] anchor_stamps=%zu cache_count=%zu "
            "machine_part=stamp%d lost_book=stamp%d\n",
            anchorStampCount, anchorCacheCount,
            NIGHT_STAMP_TYPE_MACHINE_PART, NIGHT_STAMP_TYPE_LOST_BOOK);
    }
    const size_t combinedCount = existingCount + targetCount + anchorStampCount;
    NightPointerVector transientView = {
        combined, combined + combinedCount, combined + combinedCount
    };
    NightPointerVector* previousTransientView = g_nightTransientStampVector;
    const bool previousTransientResolved =
        g_nightTransientStampVectorResolved;
    g_nightTransientStampVector = &transientView;
    g_nightTransientStampVectorResolved = false;
    g_originalNightMapRedraw(mapTask);
    const bool transientResolved = g_nightTransientStampVectorResolved;
    g_nightTransientStampVector = previousTransientView;
    g_nightTransientStampVectorResolved = previousTransientResolved;
    if (!transientResolved) {
        g_nightMapMarkersFaulted.store(true, std::memory_order_relaxed);
        Log("[NightMapMarkers] native redraw did not consume transient register "
            "view; future markers disabled safely\n");
        g_insideNightMapRedraw = false;
        return;
    }
    if (loggedRedraw != 0) {
        Log("[NightMapMarkers] draw_submitted redraw=%llu ghost_rat=%d "
            "treasure_box=%d "
            "transient_stamps=%zu existing_stamps=%zu existing_capacity=%zu "
            "native_existing_limit=%zu coordinate_domain=native_map "
            "area_filter=player_native_MapGetAreaID "
            "transient_view_consumed=1 persistent_vector_write=0 "
            "save_api_called=0 visual_confirmation_required=1\n",
            static_cast<unsigned long long>(loggedRedraw), ghostRatCount,
            treasureBoxCount, targetCount, existingCount,
            existingCapacity, NIGHT_MAX_EXISTING_STAMPS);
    }
    g_insideNightMapRedraw = false;
}

static void NightClearAnchorCache() {
    g_nightAnchorCacheCount.store(0, std::memory_order_release);
    for (size_t i = 0; i < NIGHT_ANCHOR_CACHE_CAPACITY; ++i) {
        g_nightAnchorCache[i].valid = false;
        g_nightAnchorCache[i].name[0] = '\0';
    }
    g_nightProgressive.active = false;
    g_nightProgressive.nextIndex = 0;
    g_nightProgressive.cachedCount = 0;
    Log("[NightMapMarkers][anchor] cache cleared\n");
}

static bool SetNightMapMarkers(bool enabled) {
    if (!g_nightMapMarkersAvailable ||
        g_nightMapMarkersFaulted.load(std::memory_order_relaxed)) return false;
    g_nightMapMarkersEnabled.store(enabled, std::memory_order_relaxed);
    Log("[NightMapMarkers] settings_panel %s\n", enabled ? "ENABLED" : "disabled");
    return true;
}

static bool InstallNightMapMarkers() {
    if (!g_supportedExe) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    static const unsigned char redrawExpected[NIGHT_MAP_REDRAW_HOOK_LENGTH] = {
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
        0x24, 0x18, 0x57, 0x48, 0x81, 0xEC, 0x80, 0x00,
        0x00, 0x00
    };
    static const unsigned char initializerCallExpected[5] = {
        0xE8, 0xF6, 0x3D, 0x00, 0x00
    };
    static const unsigned char vectorReadExpected[37] = {
        0x48, 0x8B, 0x05, 0x4D, 0x78, 0xE0, 0x00, 0x48,
        0x8B, 0x88, 0x08, 0x02, 0x00, 0x00, 0x48, 0x8B,
        0xB1, 0xE0, 0x35, 0x00, 0x00, 0x48, 0x8B, 0x99,
        0xD8, 0x35, 0x00, 0x00, 0x48, 0x3B, 0xDE, 0x0F,
        0x84, 0x05, 0x01, 0x00, 0x00
    };;
    static const unsigned char spatialExpected[14] = {
        0x40, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
        0x41, 0x57, 0x48, 0x83, 0xEC, 0x60
    };
    static const unsigned char rawFreeExpected[13] = {
        0x48, 0x83, 0xEC, 0x38, 0x48, 0x81, 0xFA, 0x00,
        0x10, 0x00, 0x00, 0x72, 0x14
    };
    static const unsigned char creatureConstructorExpected[31] = {
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x4C,
        0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48,
        0x8B, 0xD9, 0xE8, 0x99, 0x21, 0x03, 0x00, 0x90,
        0x48, 0x8D, 0x05, 0x29, 0x6D, 0xD4, 0x00
    };;
    static const unsigned char dataBindExpected[24] = {
        0x48, 0x8B, 0x06, 0x48, 0x89, 0x87, 0x90, 0x02,
        0x00, 0x00, 0x4C, 0x8B, 0x30, 0x44, 0x89, 0x7B,
        0x10, 0x41, 0x8B, 0xAE, 0x9C, 0x0B, 0x00, 0x00
    };
    static const unsigned char idAccessExpected[30] = {
        0x48, 0x8B, 0x86, 0x90, 0x02, 0x00, 0x00, 0x4C,
        0x8B, 0x00, 0x4D, 0x8B, 0x40, 0x10, 0x48, 0x8D,
        0x15, 0x87, 0xF4, 0xD3, 0x00, 0x48, 0x8D, 0x4D,
        0xA0, 0xE8, 0x66, 0x02, 0x59, 0x00
    };;
    static const unsigned char worldToMapExpected[27] = {
        0xF3, 0x0F, 0x10, 0x0D, 0x4C, 0xF7, 0xC6, 0x00,
        0x48, 0x8B, 0xC2, 0xF3, 0x41, 0x0F, 0x5E, 0x08,
        0x0F, 0xC6, 0xC9, 0x00, 0x0F, 0x59, 0x09, 0x0F,
        0x11, 0x0A, 0xC3
    };;
    static const unsigned char vectorAddExpected[14] = {
        0x41, 0x0F, 0x10, 0x00, 0x48, 0x8B, 0xC2, 0x0F,
        0x58, 0x01, 0x0F, 0x11, 0x02, 0xC3
    };
    static const unsigned char positionTransformChainExpected[27] = {
        0xE8, 0x56, 0xF7, 0xE2, 0xFF, 0x4C, 0x8D, 0x05,
        0x37, 0x0C, 0x74, 0x00, 0x48, 0x8D, 0x95, 0x30,
        0x14, 0x00, 0x00, 0x48, 0x8B, 0xC8, 0xE8, 0xA0,
        0x97, 0xF3, 0xFF
    };;
    static const unsigned char offsetLoadExpected[22] = {
        0xF3, 0x44, 0x0F, 0x10, 0x25, 0xD8, 0x96, 0xBA,
        0x00, 0x41, 0x0F, 0x28, 0xD4, 0xF3, 0x44, 0x0F,
        0x10, 0x0D, 0x37, 0x94, 0xBA, 0x00
    };;
    static const unsigned char statusPositionGetterExpected[8] = {
        0x48, 0x8D, 0x81, 0xF0, 0x00, 0x00, 0x00, 0xC3
    };
    static const unsigned char statusUniqueIdGetterExpected[8] = {
        0x48, 0x8B, 0x81, 0xE0, 0x00, 0x00, 0x00, 0xC3
    };
    // The Lua MapGetAreaID native takes (map-information holder, float4*) and
    // walks a 16-byte area table.  The second window pins all four inclusive
    // XY bounds checks, the zero/no-match return and the matching area-ID
    // return.  These are semantic gates, not relocation-only signatures.
    static const unsigned char areaResolverHeaderExpected[57] = {
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57,
        0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0x01, 0x45,
        0x33, 0xF6, 0x48, 0x8B, 0xF2, 0x49, 0x8B, 0x40,
        0x60, 0x49, 0x8D, 0x68, 0x68, 0x48, 0xC1, 0xE0,
        0x04, 0x49, 0x8D, 0x78, 0x68, 0x48, 0x03, 0xE8,
        0x48, 0x3B, 0xFD, 0x0F, 0x84, 0x45, 0x01, 0x00,
        0x00
    };
    static const unsigned char areaResolverBoundsExpected[120] = {
        0x48, 0x8B, 0x08, 0x66, 0x0F, 0x6E, 0x47, 0x08,
        0x66, 0x0F, 0x6E, 0x67, 0x0C, 0xF3, 0x0F, 0x10,
        0x1E, 0x66, 0x0F, 0x6E, 0x89, 0xB0, 0x01, 0x00,
        0x00, 0x66, 0x0F, 0x6E, 0x91, 0xB4, 0x01, 0x00,
        0x00, 0x0F, 0x5B, 0xC0, 0x0F, 0x5B, 0xC9, 0x0F,
        0x2F, 0xC3, 0x0F, 0x5B, 0xD2, 0x0F, 0x5B, 0xE4,
        0xF3, 0x0F, 0x58, 0xC8, 0xF3, 0x0F, 0x58, 0xD4,
        0x77, 0x14, 0x0F, 0x2F, 0xD9, 0x77, 0x0F, 0xF3,
        0x0F, 0x10, 0x46, 0x04, 0x0F, 0x2F, 0xE0, 0x77,
        0x05, 0x0F, 0x2F, 0xC2, 0x76, 0x25, 0x48, 0x83,
        0xC7, 0x10, 0x48, 0x3B, 0xFD, 0x0F, 0x85, 0xE2,
        0xFE, 0xFF, 0xFF, 0x49, 0x8B, 0xC6, 0x48, 0x8B,
        0x5C, 0x24, 0x68, 0x48, 0x83, 0xC4, 0x20, 0x41,
        0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F,
        0x5E, 0x5D, 0xC3, 0x48, 0x8B, 0xC3, 0xEB, 0xE6
    };
    // Both a player-area caller and a general status-area caller establish the
    // same ABI and the exact live-valid/live-object/map-holder fallback order.
    static const unsigned char areaPlayerChainExpected[79] = {
        0x48, 0x8B, 0x91, 0x08, 0x02, 0x00, 0x00, 0x48,
        0x8B, 0x8A, 0xB8, 0x32, 0x00, 0x00, 0x48, 0x8B,
        0x91, 0x58, 0x01, 0x00, 0x00, 0x48, 0x85, 0xD2,
        0x74, 0x22, 0x80, 0x3A, 0x00, 0x74, 0x1D, 0x4C,
        0x8B, 0x81, 0x50, 0x01, 0x00, 0x00, 0x48, 0x8D,
        0x91, 0xF0, 0x00, 0x00, 0x00, 0x4D, 0x85, 0xC0,
        0x74, 0x11, 0x49, 0x8B, 0xC8, 0xE8, 0x25, 0x39,
        0x06, 0x00, 0xEB, 0x13, 0x48, 0x8D, 0x91, 0xF0,
        0x00, 0x00, 0x00, 0x48, 0x8B, 0x89, 0xE8, 0x00,
        0x00, 0x00, 0xE8, 0x70, 0xB2, 0xFF, 0xFF
    };
    static const unsigned char areaStatusChainExpected[69] = {
        0x48, 0x8B, 0x81, 0x58, 0x01, 0x00, 0x00, 0x48,
        0x85, 0xC0, 0x74, 0x22, 0x80, 0x38, 0x00, 0x74,
        0x1D, 0x48, 0x8B, 0x81, 0x50, 0x01, 0x00, 0x00,
        0x48, 0x8D, 0x91, 0xF0, 0x00, 0x00, 0x00, 0x48,
        0x85, 0xC0, 0x74, 0x11, 0x48, 0x8B, 0xC8, 0xE8,
        0x10, 0x55, 0x0F, 0x00, 0xEB, 0x13, 0x48, 0x8D,
        0x91, 0xF0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x89,
        0xE8, 0x00, 0x00, 0x00, 0xE8, 0x5B, 0xCE, 0x08,
        0x00, 0x48, 0x3B, 0xC7, 0x0F
    };;
    static const unsigned char liveAreaHeaderExpected[67] = {
        0x4C, 0x8B, 0xDC, 0x48, 0x81, 0xEC, 0xA8, 0x00,
        0x00, 0x00, 0x48, 0x8B, 0x05, 0x2F, 0x91, 0xE4,
        0x00, 0x48, 0x33, 0xC4, 0x48, 0x89, 0x84, 0x24,
        0x90, 0x00, 0x00, 0x00, 0x49, 0xC7, 0x43, 0xC8,
        0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x49, 0x30,
        0x48, 0x8D, 0x05, 0x81, 0xF5, 0xC5, 0x00, 0x49,
        0x89, 0x43, 0x88, 0x49, 0x8D, 0x43, 0xC8, 0x49,
        0x89, 0x43, 0x90, 0x49, 0x8D, 0x43, 0x88, 0x49,
        0x89, 0x43, 0xC0
    };;
    static const unsigned char stampPositionCopyExpected[26] = {
        0xF3, 0x0F, 0x10, 0x81, 0xE4, 0x03, 0x00, 0x00,
        0xF3, 0x0F, 0x10, 0x89, 0xE0, 0x03, 0x00, 0x00,
        0x0F, 0x14, 0xC8, 0x0F, 0x57, 0xF6, 0xF2, 0x0F,
        0x10, 0xF1
    };
    // rax=(end-begin)/8; if (count >= 0x64) branch past construction.  This
    // proves the persistent vector cannot legally exceed our 100-pointer
    // stack-copy bound, independently of map/story unlock progress.
    static const unsigned char stampNativeLimitExpected[20] = {
        0x48, 0x8B, 0x47, 0x08, 0x48, 0x2B, 0x07, 0x48,
        0xC1, 0xF8, 0x03, 0x83, 0xF8, 0x64, 0x0F, 0x8D,
        0xA3, 0x02, 0x00, 0x00
    };
    // The native constructor allocates exactly 0x30 bytes at 0x20-byte
    // alignment, then writes the vtable, type at +0x10, and the complete
    // transformed float4 position at +0x20.  These bytes prove the layout
    // used by NightFakeMapStamp rather than inferring it from nearby reads.
    static const unsigned char stampAllocationExpected[11] = {
        0xBA, 0x20, 0x00, 0x00, 0x00, 0x41, 0xB8, 0x30,
        0x00, 0x00, 0x00
    };
    static const unsigned char stampLayoutWritesExpected[18] = {
        0x48, 0x8D, 0x05, 0xE2, 0x4D, 0xB3, 0x00, 0x48,
        0x89, 0x03, 0x44, 0x89, 0x73, 0x10, 0x0F, 0x11,
        0x73, 0x20
    };;
    static const unsigned char stampAtlasTypeExpected[28] = {
        0x41, 0x8B, 0x41, 0x10, 0xFF, 0xC0, 0x99, 0x83,
        0xE2, 0x03, 0x03, 0xC2, 0x44, 0x8B, 0xC0, 0x83,
        0xE0, 0x03, 0x2B, 0xC2, 0x41, 0xC1, 0xF8, 0x02,
        0x66, 0x0F, 0x6E, 0xD8
    };
    static const unsigned char stampSelectionExpected[32] = {
        0x8B, 0x50, 0x10, 0xFF, 0xCA, 0x83, 0xB9, 0xB8,
        0x03, 0x00, 0x00, 0xFF, 0x74, 0x0D, 0x83, 0xB9,
        0xB0, 0x03, 0x00, 0x00, 0x02, 0x0F, 0x84, 0x93,
        0xE1, 0xFF, 0xFF, 0xE9, 0xEE, 0xDC, 0xFF, 0xFF
    };
    static const unsigned char stampPositionReadExpected[5] = {
        0x41, 0x0F, 0x10, 0x61, 0x20
    };
    static const unsigned char stampRenderArgsExpected[47] = {
        0x48, 0x8D, 0x44, 0x24, 0x50, 0x48, 0x89, 0x44,
        0x24, 0x28, 0xC7, 0x44, 0x24, 0x20, 0xFF, 0xFF,
        0xFF, 0xFF, 0x0F, 0x57, 0xDB, 0x4C, 0x8D, 0x44,
        0x24, 0x40, 0x48, 0x8D, 0x54, 0x24, 0x30, 0x48,
        0x8B, 0x8F, 0x48, 0x05, 0x00, 0x00, 0xE8, 0xD7,
        0xB7, 0x44, 0x00, 0x48, 0x83, 0xC3, 0x08
    };
    static const unsigned char stampLoopExpected[13] = {
        0x48, 0x83, 0xC3, 0x08, 0x48, 0x3B, 0xDE, 0x0F,
        0x85, 0x0B, 0xFF, 0xFF, 0xFF
    };
    static const unsigned char rendererR9OverwriteExpected[12] = {
        0x8B, 0x84, 0x24, 0xD0, 0x00, 0x00, 0x00, 0x4C,
        0x8D, 0x4C, 0x24, 0x30
    };
    static const unsigned char enabledExpected[13] = {
        0x48, 0x8B, 0x4B, 0x10, 0x80, 0xB9, 0x30, 0x01,
        0x00, 0x00, 0x00, 0x74, 0x48
    };
    static const unsigned char membershipExpected[51] = {
        0x48, 0x8B, 0x81, 0xE8, 0x00, 0x00, 0x00, 0x48,
        0x85, 0xC0, 0x74, 0x08, 0x48, 0x8B, 0x00, 0x4C,
        0x8B, 0x00, 0xEB, 0x03, 0x4C, 0x8B, 0xC5, 0x48,
        0x8B, 0x87, 0x68, 0x05, 0x00, 0x00, 0x48, 0x85,
        0xC0, 0x74, 0x08, 0x48, 0x8B, 0x00, 0x48, 0x8B,
        0x10, 0xEB, 0x03, 0x48, 0x8B, 0xD5, 0x4C, 0x3B,
        0xC2, 0x75, 0x15
    };
    static const unsigned char spatialOwnerChainExpected[33] = {
        0x48, 0x8B, 0x05, 0xD6, 0xC5, 0x01, 0x01, 0x48,
        0x8B, 0x88, 0x68, 0x02, 0x00, 0x00, 0x48, 0x8B,
        0x89, 0xD8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD7,
        0x48, 0x8B, 0x49, 0x30, 0xE8, 0xCC, 0xDA, 0x0A,
        0x00
    };;
    static const unsigned char spatialAdmissionExpected[15] = {
        0x48, 0x8B, 0x4E, 0x30, 0x41, 0xB0, 0x01, 0x48,
        0x8B, 0xD3, 0xE8, 0xFB, 0x18, 0x0A, 0x00
    };
    static const unsigned char spatialIndexInsertExpected[23] = {
        0x48, 0x8D, 0x8F, 0xE0, 0x06, 0x00, 0x00, 0x66,
        0x0F, 0x7F, 0x74, 0x24, 0x20, 0x4C, 0x8D, 0x44,
        0x24, 0x20, 0xE8, 0x8C, 0xC9, 0xF8, 0xFF
    };
    static const unsigned char divisorExpected[4] = {
        0x4E, 0x62, 0x04, 0x41
    };
    static const unsigned char oneExpected[4] = {
        0x00, 0x00, 0x80, 0x3F
    };
    static const unsigned char offsetXExpected[4] = {
        0x00, 0x00, 0xAE, 0x43
    };
    static const unsigned char offsetYExpected[4] = {
        0x00, 0x00, 0x11, 0xC3
    };
    void* spatialEntry = g_originalSpatialTest
        ? reinterpret_cast<void*>(g_originalSpatialTest)
        : reinterpret_cast<void*>(base + RVA_NIGHT_SPATIAL_SEARCH);
    void** mapTaskVtable = reinterpret_cast<void**>(base + RVA_NIGHT_MAP_TASK_VTABLE);
    void** stampVtable = reinterpret_cast<void**>(base + RVA_NIGHT_MAP_STAMP_VTABLE);
    void** creatureVtable = reinterpret_cast<void**>(base + RVA_NIGHT_CREATURE_VTABLE);
    void** callbackVtable = reinterpret_cast<void**>(
        base + RVA_NIGHT_CREATURE_CALLBACK_VTABLE);
    const bool semanticsOk =
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_MAP_REDRAW),
               redrawExpected, sizeof(redrawExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_MAP_INITIALIZER_CALL),
               initializerCallExpected, sizeof(initializerCallExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_MAP_VECTOR_READ),
               vectorReadExpected, sizeof(vectorReadExpected)) == 0 &&
        memcmp(spatialEntry, spatialExpected, sizeof(spatialExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_RAW_VECTOR_FREE),
               rawFreeExpected, sizeof(rawFreeExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_CREATURE_CONSTRUCTOR),
               creatureConstructorExpected, sizeof(creatureConstructorExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_CREATURE_DATA_BIND),
               dataBindExpected, sizeof(dataBindExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_CREATURE_ID_ACCESS),
               idAccessExpected, sizeof(idAccessExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_WORLD_TO_MAP),
               worldToMapExpected, sizeof(worldToMapExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_VECTOR_ADD),
               vectorAddExpected, sizeof(vectorAddExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_STATUS_POSITION_GETTER),
               statusPositionGetterExpected,
               sizeof(statusPositionGetterExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base +
                                      RVA_NIGHT_STATUS_UNIQUE_ID_GETTER),
               statusUniqueIdGetterExpected,
               sizeof(statusUniqueIdGetterExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_MAP_AREA_RESOLVER),
               areaResolverHeaderExpected,
               sizeof(areaResolverHeaderExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base +
                                      RVA_NIGHT_MAP_AREA_RESOLVER_BOUNDS),
               areaResolverBoundsExpected,
               sizeof(areaResolverBoundsExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_LIVE_AREA_RESOLVER),
               liveAreaHeaderExpected, sizeof(liveAreaHeaderExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_MAP_AREA_PLAYER_CHAIN),
               areaPlayerChainExpected,
               sizeof(areaPlayerChainExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_MAP_AREA_STATUS_CHAIN),
               areaStatusChainExpected,
               sizeof(areaStatusChainExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_POSITION_GETTER_CALL),
               positionTransformChainExpected,
               sizeof(positionTransformChainExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + 0x2E9E63),
               offsetLoadExpected, sizeof(offsetLoadExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_MAP_STAMP_POSITION_COPY),
               stampPositionCopyExpected,
               sizeof(stampPositionCopyExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_MAP_STAMP_NATIVE_LIMIT),
               stampNativeLimitExpected,
               sizeof(stampNativeLimitExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_MAP_STAMP_ALLOCATION),
               stampAllocationExpected,
               sizeof(stampAllocationExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_MAP_STAMP_LAYOUT_WRITES),
               stampLayoutWritesExpected,
               sizeof(stampLayoutWritesExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_MAP_STAMP_ATLAS_TYPE),
               stampAtlasTypeExpected, sizeof(stampAtlasTypeExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_MAP_STAMP_SELECTION),
               stampSelectionExpected, sizeof(stampSelectionExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_STAMP_POSITION_READ),
               stampPositionReadExpected,
               sizeof(stampPositionReadExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_STAMP_RENDER_ARGS),
               stampRenderArgsExpected, sizeof(stampRenderArgsExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_STAMP_LOOP_ADVANCE),
               stampLoopExpected, sizeof(stampLoopExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_RENDERER_R9_OVERWRITE),
               rendererR9OverwriteExpected,
               sizeof(rendererR9OverwriteExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base +
                   RVA_NIGHT_ENABLED_MEMBERSHIP_PROOF),
               enabledExpected, sizeof(enabledExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base +
                   RVA_NIGHT_ENABLED_MEMBERSHIP_PROOF + 0x0d),
               membershipExpected, sizeof(membershipExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_SPATIAL_OWNER_CHAIN),
               spatialOwnerChainExpected,
               sizeof(spatialOwnerChainExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_SPATIAL_ADMISSION_WINDOW),
               spatialAdmissionExpected,
               sizeof(spatialAdmissionExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base +
                   RVA_NIGHT_SPATIAL_INDEX_INSERT_WINDOW),
               spatialIndexInsertExpected,
               sizeof(spatialIndexInsertExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_MAP_DIVISOR),
               divisorExpected, sizeof(divisorExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_MAP_SCALE_ONE),
               oneExpected, sizeof(oneExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_MAP_OFFSET_X),
               offsetXExpected, sizeof(offsetXExpected)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_MAP_OFFSET_Y),
               offsetYExpected, sizeof(offsetYExpected)) == 0 &&
        CodeDirectCallTargets(base + RVA_NIGHT_POSITION_GETTER_CALL,
                              base + RVA_NIGHT_STATUS_POSITION_GETTER) &&
        CodeDirectCallTargets(base + RVA_NIGHT_MAP_AREA_PLAYER_LIVE_CALL,
                              base + RVA_NIGHT_LIVE_AREA_RESOLVER) &&
        CodeDirectCallTargets(base + RVA_NIGHT_MAP_AREA_PLAYER_CALL,
                              base + RVA_NIGHT_MAP_AREA_RESOLVER) &&
        CodeDirectCallTargets(base + RVA_NIGHT_MAP_AREA_STATUS_LIVE_CALL,
                              base + RVA_NIGHT_LIVE_AREA_RESOLVER) &&
        CodeDirectCallTargets(base + RVA_NIGHT_MAP_AREA_STATUS_CALL,
                              base + RVA_NIGHT_MAP_AREA_RESOLVER) &&
        CodeDirectCallTargets(base + RVA_NIGHT_WORLD_TO_MAP_CALL,
                              base + RVA_NIGHT_WORLD_TO_MAP) &&
        CodeDirectCallTargets(base + RVA_NIGHT_VECTOR_ADD_CALL,
                              base + RVA_NIGHT_VECTOR_ADD) &&
        CodeDirectCallTargets(base + RVA_NIGHT_SPATIAL_OWNER_LOOKUP_CALL,
                              base + RVA_NIGHT_SPATIAL_OWNER_LOOKUP) &&
        CodeDirectCallTargets(base + RVA_NIGHT_SPATIAL_ADMISSION_CALL,
                              base + RVA_NIGHT_SPATIAL_ADMISSION) &&
        CodeDirectCallTargets(base + RVA_NIGHT_SPATIAL_INDEX_INSERT_CALL,
                              base + RVA_NIGHT_SPATIAL_INDEX_INSERT) &&
        CodeDirectCallTargets(base + RVA_NIGHT_MEMBERSHIP_ADMISSION_CALL,
                              base + RVA_NIGHT_SPATIAL_ADMISSION) &&
        CodeDirectCallTargets(base + RVA_NIGHT_STAMP_RENDER_CALL,
                              base + RVA_NIGHT_STAMP_RENDERER) &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_CREATURE_ID_FORMAT),
               "pop_sensor_%s", sizeof("pop_sensor_%s")) == 0 &&
        NightCheckType(base, RVA_NIGHT_MAP_TASK_VTABLE,
                       RVA_NIGHT_MAP_TASK_COL, RVA_NIGHT_MAP_TASK_TYPE,
                       ".?AVCTask_MainMenu_Map@@") &&
        NightCheckType(base, RVA_NIGHT_MAP_STAMP_VTABLE,
                       RVA_NIGHT_MAP_STAMP_COL, RVA_NIGHT_MAP_STAMP_TYPE,
                       ".?AVCMapStampStatus@@") &&
        NightCheckType(base, RVA_NIGHT_CREATURE_VTABLE,
                       RVA_NIGHT_CREATURE_COL, RVA_NIGHT_CREATURE_TYPE,
                       ".?AVCCreatureStatus@@") &&
        NightCheckType(base, RVA_NIGHT_MAP_INFORMATION_VTABLE,
                       RVA_NIGHT_MAP_INFORMATION_COL,
                       RVA_NIGHT_MAP_INFORMATION_TYPE,
                       ".?AVCMapInformation@@") &&
        NightCheckType(base, RVA_NIGHT_COM_MAP_VTABLE,
                       RVA_NIGHT_COM_MAP_COL, RVA_NIGHT_COM_MAP_TYPE,
                       ".?AVCCom_Map@@") &&
        NightCheckType(base, RVA_NIGHT_PLAYER_STATUS_VTABLE,
                       RVA_NIGHT_PLAYER_STATUS_COL,
                       RVA_NIGHT_PLAYER_STATUS_TYPE,
                       ".?AVCPlayerStatus@@") &&
        *reinterpret_cast<void**>(base + RVA_NIGHT_CREATURE_CALLBACK_VTABLE - 8) ==
            reinterpret_cast<void*>(base + RVA_NIGHT_CREATURE_CALLBACK_COL) &&
        IsReadable(mapTaskVtable, sizeof(void*) * 30) &&
        mapTaskVtable[29] == reinterpret_cast<void*>(
            base + RVA_NIGHT_MAP_TASK_INITIALIZE) &&
        IsReadable(stampVtable, sizeof(void*) * 5) &&
        stampVtable[0] == reinterpret_cast<void*>(base + RVA_NIGHT_MAP_STAMP_DESTROY) &&
        IsReadable(creatureVtable, sizeof(void*) * 5) &&
        creatureVtable[0] == reinterpret_cast<void*>(base + RVA_NIGHT_CREATURE_DESTROY) &&
        IsReadable(callbackVtable, sizeof(void*) * 5) &&
        callbackVtable[0] == reinterpret_cast<void*>(
            base + RVA_NIGHT_CREATURE_CALLBACK_COPY) &&
        callbackVtable[2] == reinterpret_cast<void*>(
            base + RVA_NIGHT_CREATURE_CALLBACK_INVOKE) &&
        callbackVtable[4] == reinterpret_cast<void*>(
            base + RVA_NIGHT_CREATURE_CALLBACK_DESTROY);
    if (!semanticsOk) {
        Log("[NightMapMarkers] instruction/RTTI/vtable/call-chain check FAILED; "
            "feature disabled safely\n");
        return false;
    }

    unsigned char* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!trampoline) {
        Log("[NightMapMarkers] redraw trampoline allocation failed (error %lu)\n",
            GetLastError());
        return false;
    }
    memcpy(trampoline, redrawExpected, sizeof(redrawExpected));
    unsigned char jumpBack[14] = {0xff, 0x25, 0, 0, 0, 0};
    *reinterpret_cast<u64*>(jumpBack + 6) =
        base + RVA_NIGHT_MAP_REDRAW + NIGHT_MAP_REDRAW_HOOK_LENGTH;
    memcpy(trampoline + NIGHT_MAP_REDRAW_HOOK_LENGTH, jumpBack, sizeof(jumpBack));
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);

    unsigned char* rangeRelay = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!rangeRelay) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        Log("[NightMapMarkers] stamp-range relay allocation failed (error %lu)\n",
            GetLastError());
        return false;
    }
    // The hook replaces only:
    //   mov rsi,[rcx+35e0]; mov rbx,[rcx+35d8]
    // It resolves a TLS view for this redraw, loads end/begin into the same
    // nonvolatile registers, and jumps back to 0x2e6188.  No game vector
    // header is ever modified.
    unsigned char rangeRelayCode[41] = {
        0x48, 0x83, 0xec, 0x20,
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,
        0xff, 0xd0,
        0x48, 0x83, 0xc4, 0x20,
        0x48, 0x8b, 0x70, 0x08,
        0x48, 0x8b, 0x18,
        0xff, 0x25, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0
    };
    *reinterpret_cast<u64*>(rangeRelayCode + 6) =
        reinterpret_cast<u64>(&NightResolveStampVector);
    *reinterpret_cast<u64*>(rangeRelayCode + 33) =
        base + RVA_NIGHT_MAP_VECTOR_RANGE_LOAD +
        NIGHT_MAP_VECTOR_RANGE_HOOK_LENGTH;
    memcpy(rangeRelay, rangeRelayCode, sizeof(rangeRelayCode));
    if (!SealExecutableMemory(trampoline, 64) ||
        !SealExecutableMemory(rangeRelay, 64)) {
        VirtualFree(rangeRelay, 0, MEM_RELEASE);
        VirtualFree(trampoline, 0, MEM_RELEASE);
        Log("[NightMapMarkers] trampoline/relay RX protection failed; "
            "feature disabled safely\n");
        return false;
    }

    g_originalNightMapRedraw =
        reinterpret_cast<NightMapRedrawFunction>(trampoline);
    g_nightMapRedrawTrampoline = trampoline;
    g_nightStampRangeRelay = rangeRelay;
    g_nightSpatialSearch = reinterpret_cast<NightSpatialSearchFunction>(spatialEntry);
    g_nightRawVectorFree = reinterpret_cast<NightRawVectorFreeFunction>(
        base + RVA_NIGHT_RAW_VECTOR_FREE);
    g_nightWorldToMap = reinterpret_cast<NightVectorTransformFunction>(
        base + RVA_NIGHT_WORLD_TO_MAP);
    g_nightVectorAdd = reinterpret_cast<NightVectorTransformFunction>(
        base + RVA_NIGHT_VECTOR_ADD);
    g_nightMapAreaResolver = reinterpret_cast<NightMapAreaResolverFunction>(
        base + RVA_NIGHT_MAP_AREA_RESOLVER);
    g_nightLiveAreaResolver =
        reinterpret_cast<NightMapAreaResolverFunction>(
            base + RVA_NIGHT_LIVE_AREA_RESOLVER);
    g_nightAnchorSearchDirect = reinterpret_cast<NightExactAnchorSearchFunc>(
        base + RVA_NIGHT_EXACT_ANCHOR_SEARCH);
    g_nightMapHashLookup = reinterpret_cast<NightMapHashLookupFunc>(
        base + RVA_NIGHT_MAP_HASH_LOOKUP);
    g_nightSaveDataAccessor = reinterpret_cast<NightSaveDataAccessorFunc>(
        base + RVA_NIGHT_SAVE_DATA_ACCESSOR);

    static const unsigned char vectorRangeOriginal[
        NIGHT_MAP_VECTOR_RANGE_HOOK_LENGTH] = {
        0x48, 0x8b, 0xb1, 0xe0, 0x35, 0x00, 0x00,
        0x48, 0x8b, 0x99, 0xd8, 0x35, 0x00, 0x00
    };
    unsigned char rangeHook[NIGHT_MAP_VECTOR_RANGE_HOOK_LENGTH] = {
        0xff, 0x25, 0, 0, 0, 0
    };
    *reinterpret_cast<u64*>(rangeHook + 6) =
        reinterpret_cast<u64>(rangeRelay);

    unsigned char hook[NIGHT_MAP_REDRAW_HOOK_LENGTH] = {
        0xff, 0x25, 0, 0, 0, 0
    };
    *reinterpret_cast<u64*>(hook + 6) =
        reinterpret_cast<u64>(&NightMapRedrawDetour);
    memset(hook + 14, 0x90, NIGHT_MAP_REDRAW_HOOK_LENGTH - 14);
    if (!CodeRangeHasExpectedProtection(
            reinterpret_cast<void*>(base + RVA_NIGHT_MAP_VECTOR_RANGE_LOAD),
            sizeof(rangeHook)) ||
        !CodeRangeHasExpectedProtection(
            reinterpret_cast<void*>(base + RVA_NIGHT_MAP_REDRAW),
            sizeof(hook))) {
        VirtualFree(rangeRelay, 0, MEM_RELEASE);
        VirtualFree(trampoline, 0, MEM_RELEASE);
        g_originalNightMapRedraw = nullptr;
        g_nightMapRedrawTrampoline = nullptr;
        g_nightStampRangeRelay = nullptr;
        g_nightSpatialSearch = nullptr;
        g_nightRawVectorFree = nullptr;
        g_nightWorldToMap = nullptr;
        g_nightVectorAdd = nullptr;
        g_nightMapAreaResolver = nullptr;
        g_nightLiveAreaResolver = nullptr;
        Log("[NightMapMarkers] patch page protection check FAILED; "
            "feature disabled safely\n");
        return false;
    }
    bool anyWriteAttempted = false;
    const bool rangeWriteReported = WriteCodePatchChecked(
        reinterpret_cast<void*>(base + RVA_NIGHT_MAP_VECTOR_RANGE_LOAD),
        rangeHook, sizeof(rangeHook));
    const bool rangePatched = rangeWriteReported &&
        memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_MAP_VECTOR_RANGE_LOAD),
               rangeHook, sizeof(rangeHook)) == 0;
    bool redrawWriteReported = false;
    bool redrawPatched = false;
    if (rangePatched) {
        anyWriteAttempted = true;
        redrawWriteReported = WriteCodePatchChecked(
            reinterpret_cast<void*>(base + RVA_NIGHT_MAP_REDRAW),
            hook, sizeof(hook));
        redrawPatched = redrawWriteReported &&
            memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_MAP_REDRAW),
                   hook, sizeof(hook)) == 0;
    }
    if (!rangePatched || !redrawPatched) {
        bool rollbackWriteOk = true;
        if (memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_MAP_REDRAW),
                   redrawExpected, sizeof(redrawExpected)) != 0) {
            rollbackWriteOk = WriteCodePatchChecked(
                reinterpret_cast<void*>(base + RVA_NIGHT_MAP_REDRAW),
                redrawExpected, sizeof(redrawExpected)) && rollbackWriteOk;
        }
        if (memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_MAP_VECTOR_RANGE_LOAD),
                   vectorRangeOriginal, sizeof(vectorRangeOriginal)) != 0) {
            rollbackWriteOk = WriteCodePatchChecked(
                reinterpret_cast<void*>(base + RVA_NIGHT_MAP_VECTOR_RANGE_LOAD),
                vectorRangeOriginal, sizeof(vectorRangeOriginal)) &&
                rollbackWriteOk;
        }
        const bool redrawRestored = memcmp(
            reinterpret_cast<void*>(base + RVA_NIGHT_MAP_REDRAW),
            redrawExpected, sizeof(redrawExpected)) == 0;
        const bool rangeRestored = memcmp(
            reinterpret_cast<void*>(base + RVA_NIGHT_MAP_VECTOR_RANGE_LOAD),
            vectorRangeOriginal, sizeof(vectorRangeOriginal)) == 0;
        const bool rollbackOk = rollbackWriteOk && redrawRestored && rangeRestored;
        if (!redrawRestored)
            Log("[NightMapMarkers] rollback unresolved RVA=0x%llx\n",
                static_cast<unsigned long long>(RVA_NIGHT_MAP_REDRAW));
        if (!rangeRestored)
            Log("[NightMapMarkers] rollback unresolved RVA=0x%llx\n",
                static_cast<unsigned long long>(RVA_NIGHT_MAP_VECTOR_RANGE_LOAD));
        const bool retainExecutableMemory = anyWriteAttempted || rangeWriteReported ||
            redrawWriteReported || !rangeRestored || !redrawRestored;
        if (!retainExecutableMemory) {
            VirtualFree(rangeRelay, 0, MEM_RELEASE);
            VirtualFree(trampoline, 0, MEM_RELEASE);
            g_originalNightMapRedraw = nullptr;
            g_nightMapRedrawTrampoline = nullptr;
            g_nightStampRangeRelay = nullptr;
            g_nightSpatialSearch = nullptr;
            g_nightRawVectorFree = nullptr;
            g_nightWorldToMap = nullptr;
            g_nightVectorAdd = nullptr;
            g_nightMapAreaResolver = nullptr;
            g_nightLiveAreaResolver = nullptr;
        }
        g_nightMapMarkersAvailable = false;
        g_nightMapMarkersEnabled.store(false, std::memory_order_relaxed);
        Log("[NightMapMarkers] two-site hook write/readback failed; "
            "rollback=%s executable_memory_retained=%d; disabled safely\n",
            rollbackOk ? "OK" : "FAIL",
            retainExecutableMemory ? 1 : 0);
        return false;
    }
    g_nightMapMarkersAvailable = true;
    g_nightMapMarkersEnabled.store(true, std::memory_order_relaxed);
    Log("[NightMapMarkers] ready: Ghost Rat=stamp0 Treasure Box=stamp1; "
        "actual_enabled_status_only=1 candidate_anchor_injection=0; "
        "map_assets=5 ghost_spawn_anchors=30 treasure_spawn_anchors=11 "
        "ghost_waypoints=100 expected_live_ghost_max=8 capacity=64; "
        "native world/8.274+(348,-145) transform; "
        "native creature spatial search; register-view redraw injection; "
        "area_filter=player_native_live_or_MapGetAreaID "
        "area_cross_domain_rejected=1; "
        "stamp_position=XY00 world_transform_mulps_alignment=16 "
        "persistent_vector_write=0 save_api_called=0; "
        "impl=map_register_view_xy00_v6_native_area_filter range_hook=1; "
        "readable_cache_scope=single_collect no_cross_frame_status_cache=1 "
        "bounded_scalar_target_observations=64; "
        "starts ENABLED\n");
    return true;
}

// ===========================================================================
// InstallNightAnchorPosHook: verify 0x1C1A00 signature and set up direct
// call pointer for active anchor scanning (no passive hook needed).
// ===========================================================================
static bool InstallNightAnchorPosHook() {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

    // Verify the function prologue at 0x1C1A00
    static const unsigned char anchorExpected[NIGHT_EXACT_ANCHOR_HOOK_LENGTH] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x54,
        0x24, 0x10, 0x55, 0x56, 0x57, 0x48
    };
    if (memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_EXACT_ANCHOR_SEARCH),
               anchorExpected, sizeof(anchorExpected)) != 0) {
        Log("[NightMapMarkers] exact anchor signature mismatch at RVA=0x%llx; "
            "active scan disabled\n",
            static_cast<unsigned long long>(RVA_NIGHT_EXACT_ANCHOR_SEARCH));
        return false;
    }

    // Also verify the full sub rsp instruction (bytes 13..19)
    static const unsigned char subRspExpected[7] = {
        0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00
    };
    if (memcmp(reinterpret_cast<void*>(base + RVA_NIGHT_EXACT_ANCHOR_SEARCH + 13),
               subRspExpected, sizeof(subRspExpected)) != 0) {
        Log("[NightMapMarkers] exact anchor sub-rsp mismatch; "
            "active scan disabled\n");
        return false;
    }

    // No hook installation — we call 0x1C1A00 directly via g_nightAnchorSearchDirect
    // (already set in InstallNightMapMarkers).  The passive hook approach (scheme G)
    // was proven ineffective: book/gear anchors are queried at save-load time before
    // the MOD DLL is loaded.  Active scanning calls the function during map redraw
    // when the game world is fully initialized.
    Log("[NightMapMarkers] active anchor scan ready at RVA=0x%llx "
        "(no passive hook; direct call mode)\n",
        static_cast<unsigned long long>(RVA_NIGHT_EXACT_ANCHOR_SEARCH));
    return true;
}

// ===========================================================================
// [DIAG] MonsterMark F6 brute-force callback scan (temporary diagnostic)
// ===========================================================================
// ---------------------------------------------------------------------------
// Diagnostic-only (F6): brute-force scan of the 47 candidate callback vtables
// that pair with the spatial-search destroy helper (RVA 0x18C290).
//
// For every candidate we run the native spatial query with the same callback
// layout used by the creature search and log:
//   - callback entry RVAs (copy/invoke/destroy)
//   - result count
//   - per-object (first 8): object vtable RVA, type enum, stable id, position
// The goal is to identify which callback filters CGimmickStatus objects so
// that live book / machine-part coordinates can be pinned down.
// ---------------------------------------------------------------------------
// NIGHT_DIAG_CALLBACKS / NIGHT_DIAG_CALLBACK_COUNT are defined near the top
// of this file (next to NightLiveItem) and shared with
// NightCollectLiveNightItems.
static constexpr size_t NIGHT_DIAG_OBJECTS_PER_CALLBACK = 64;
static constexpr size_t NIGHT_DIAG_MAX_OBJECT_LINES = 2000;

static uint32_t NightDiagReadTypeEnum(void* status) {
    if (!status ||
        !IsReadable(status, NIGHT_CREATURE_DATA_OFFSET + sizeof(void*)))
        return 0xFFFFFFFFu;
    void* holder = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(status) + NIGHT_CREATURE_DATA_OFFSET);
    if (!holder || !IsReadable(holder, sizeof(void*))) return 0xFFFFFFFFu;
    void* data = *reinterpret_cast<void**>(holder);
    if (!data || !IsReadable(data, sizeof(void*))) return 0xFFFFFFFFu;
    void* vtable = *reinterpret_cast<void**>(data);
    if (!vtable || !IsReadable(vtable, 0x84 + sizeof(uint32_t)))
        return 0xFFFFFFFFu;
    return *reinterpret_cast<const uint32_t*>(
        reinterpret_cast<uintptr_t>(vtable) + 0x84);
}

static void NightDiagLogObject(uint32_t callbackRva, size_t index,
                               void* status, uintptr_t base) {
    if (!status || !IsReadable(status, sizeof(void*))) return;
    void* objVtable = *reinterpret_cast<void**>(status);
    const uintptr_t vtableRva = objVtable
        ? reinterpret_cast<uintptr_t>(objVtable) - base : 0;
    const uint32_t typeEnum = NightDiagReadTypeEnum(status);
    u64 objectId = 0;
    if (IsReadable(reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(status) +
            NIGHT_MAP_OBJECT_UNIQUE_ID_OFFSET), sizeof(u64))) {
        objectId = *reinterpret_cast<const u64*>(
            reinterpret_cast<uintptr_t>(status) +
            NIGHT_MAP_OBJECT_UNIQUE_ID_OFFSET);
    }
    float x = 0.0f;
    float y = 0.0f;
    bool posOk = false;
    if (IsReadable(reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(status) +
            NIGHT_CREATURE_POSITION_OFFSET), sizeof(float) * 4)) {
        const float* pos = reinterpret_cast<const float*>(
            reinterpret_cast<uintptr_t>(status) +
            NIGHT_CREATURE_POSITION_OFFSET);
        if (std::isfinite(pos[0]) && std::isfinite(pos[1])) {
            x = pos[0];
            y = pos[1];
            posOk = true;
        }
    }
    // Read baseID: status+0x240 -> holder -> data -> u64 baseID
    u64 baseId = 0;
    if (IsReadable(reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(status) + 0x240), sizeof(void*))) {
        void* holder = *reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(status) + 0x240);
        if (holder && IsReadable(holder, sizeof(void*))) {
            void* data = *reinterpret_cast<void**>(holder);
            if (data && IsReadable(data, sizeof(u64))) {
                baseId = *reinterpret_cast<const u64*>(data);
            }
        }
    }
    if (posOk) {
        Log("[NightMapMarkers][diag] obj cb=0x%X idx=%zu status=%p "
            "vtable=0x%llX type=0x%X%s oid=0x%llX baseID=%llu pos=(%.2f,%.2f)\n",
            callbackRva, index, status,
            static_cast<unsigned long long>(vtableRva),
            typeEnum, typeEnum == 0xFFFFFFFFu ? " GIMMICK" : "",
            static_cast<unsigned long long>(objectId),
            static_cast<unsigned long long>(baseId), x, y);
    } else {
        Log("[NightMapMarkers][diag] obj cb=0x%X idx=%zu status=%p "
            "vtable=0x%llX type=0x%X%s oid=0x%llX baseID=%llu pos=(none,none)\n",
            callbackRva, index, status,
            static_cast<unsigned long long>(vtableRva),
            typeEnum, typeEnum == 0xFFFFFFFFu ? " GIMMICK" : "",
            static_cast<unsigned long long>(objectId),
            static_cast<unsigned long long>(baseId));
    }
}

static void NightDiagScanCallbacks() {
    if (!g_nightSpatialSearch || !g_nightRawVectorFree) {
        Log("[NightMapMarkers][diag] spatial search not installed - abort\n");
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    void* root = nullptr;
    void* mapOwner = nullptr;
    void* mapInfo = nullptr;
    void* spatialOwner = nullptr;
    if (!NightReadPointer(reinterpret_cast<void*>(base + RVA_GAME_ROOT), 0, &root) ||
        !NightReadPointer(root, NIGHT_ROOT_MAP_OWNER_OFFSET, &mapOwner) ||
        !NightReadPointer(mapOwner, NIGHT_MAP_INFO_OFFSET, &mapInfo) ||
        !NightReadPointer(mapInfo, NIGHT_MAP_SPATIAL_OWNER_OFFSET,
                          &spatialOwner)) {
        Log("[NightMapMarkers][diag] spatial chain unavailable\n");
        return;
    }
    void* spatialIndex = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(spatialOwner) +
        NIGHT_MAP_SPATIAL_INDEX_OFFSET);
    if (!IsReadable(spatialIndex, 0x48)) {
        Log("[NightMapMarkers][diag] spatial index unreadable\n");
        return;
    }
    const NightRect bounds = *reinterpret_cast<const NightRect*>(
        reinterpret_cast<uintptr_t>(spatialIndex) + 0x10);
    Log("[NightMapMarkers][diag] callback_scan_start "
        "bounds=(%.2f,%.2f,%.2f,%.2f) candidates=%zu\n",
        bounds.minimumX, bounds.minimumY, bounds.maximumX, bounds.maximumY,
        NIGHT_DIAG_CALLBACK_COUNT);
    size_t objectLines = 0;
    for (size_t ci = 0; ci < NIGHT_DIAG_CALLBACK_COUNT; ++ci) {
        const uint32_t vtableRva = NIGHT_DIAG_CALLBACKS[ci];
        NightPointerVector results = {};
        u64 filterId = 0;
        NightSearchCallback callback = {};
        *reinterpret_cast<void**>(callback.storage) =
            reinterpret_cast<void*>(base + vtableRva);
        *reinterpret_cast<u64**>(callback.storage + 0x08) = &filterId;
        *reinterpret_cast<NightPointerVector**>(callback.storage + 0x10) =
            &results;
        callback.target = callback.storage;

        bool searched = false;
        __try {
            searched = g_nightSpatialSearch(spatialIndex, &bounds,
                                            &callback, -1, -1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            searched = false;
        }

        if (callback.target) {
            void** vtable = IsReadable(callback.target, sizeof(void*))
                ? *reinterpret_cast<void***>(callback.target) : nullptr;
            if (vtable && IsReadable(vtable, sizeof(void*) * 5)) {
                using DestroyFunction = void (__fastcall *)(void*, bool);
                __try {
                    reinterpret_cast<DestroyFunction>(vtable[4])(
                        callback.target, callback.target != callback.storage);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
            callback.target = nullptr;
        }

        const uintptr_t rBegin = reinterpret_cast<uintptr_t>(results.begin);
        const uintptr_t rEnd = reinterpret_cast<uintptr_t>(results.end);
        const uintptr_t rCap = reinterpret_cast<uintptr_t>(results.capacity);
        const bool vectorOk = (!rBegin && !rEnd && !rCap) ||
            (rBegin && rEnd >= rBegin && rCap >= rEnd &&
             (rEnd - rBegin) % sizeof(void*) == 0 &&
             (rCap - rBegin) % sizeof(void*) == 0);
        const size_t resultCount = vectorOk && rBegin
            ? static_cast<size_t>((rEnd - rBegin) / sizeof(void*)) : 0;

        void* entryVtable = reinterpret_cast<void*>(base + vtableRva);
        uintptr_t e0 = 0;
        uintptr_t e2 = 0;
        uintptr_t e4 = 0;
        if (IsReadable(entryVtable, sizeof(void*) * 5)) {
            e0 = reinterpret_cast<uintptr_t>(
                     *reinterpret_cast<void**>(entryVtable)) - base;
            e2 = reinterpret_cast<uintptr_t>(
                     *reinterpret_cast<void**>(
                         reinterpret_cast<uintptr_t>(entryVtable) + 0x10)) - base;
            e4 = reinterpret_cast<uintptr_t>(
                     *reinterpret_cast<void**>(
                         reinterpret_cast<uintptr_t>(entryVtable) + 0x20)) - base;
        }

        Log("[NightMapMarkers][diag] cb=0x%X searched=%d count=%zu "
            "entries[0]=0x%llX entries[2]=0x%llX entries[4]=0x%llX\n",
            vtableRva, searched ? 1 : 0, resultCount,
            static_cast<unsigned long long>(e0),
            static_cast<unsigned long long>(e2),
            static_cast<unsigned long long>(e4));

        if (vectorOk && resultCount) {
            // Special handling for cb=0xE1C168: scan ALL objects, build
            // baseID frequency table + collect sample itemNum & position
            if (vtableRva == 0xE1C168) {
                // Frequency table: up to 128 distinct baseIDs
                struct BaseIdEntry {
                    u64 baseId;
                    uint32_t count;
                    int32_t firstItemNum;  // status+0x288
                    float firstX, firstY;
                };
                BaseIdEntry freqTable[128] = {};
                size_t freqUsed = 0;
                size_t nonZeroBaseIdCount = 0;
                size_t zeroBaseIdCount = 0;

                for (size_t oi = 0; oi < resultCount; ++oi) {
                    void* s = results.begin[oi];
                    if (!s || !IsReadable(s, 0x290 + sizeof(uint32_t)))
                        continue;

                    // Read baseID via status+0x240 -> holder -> data -> u64
                    u64 bid = 0;
                    if (IsReadable(reinterpret_cast<void*>(
                            reinterpret_cast<uintptr_t>(s) + 0x240),
                            sizeof(void*))) {
                        void* holder = *reinterpret_cast<void**>(
                            reinterpret_cast<uintptr_t>(s) + 0x240);
                        if (holder && IsReadable(holder, sizeof(void*))) {
                            void* data = *reinterpret_cast<void**>(holder);
                            if (data && IsReadable(data, sizeof(u64))) {
                                bid = *reinterpret_cast<const u64*>(data);
                            }
                        }
                    }

                    // Read itemNum from status+0x288 (dword)
                    int32_t itemNum = -1;
                    if (IsReadable(reinterpret_cast<void*>(
                            reinterpret_cast<uintptr_t>(s) + 0x288),
                            sizeof(uint32_t))) {
                        itemNum = *reinterpret_cast<const int32_t*>(
                            reinterpret_cast<uintptr_t>(s) + 0x288);
                    }

                    // Read position
                    float px = 0.0f, py = 0.0f;
                    bool posOk = false;
                    if (IsReadable(reinterpret_cast<void*>(
                            reinterpret_cast<uintptr_t>(s) +
                            NIGHT_CREATURE_POSITION_OFFSET),
                            sizeof(float) * 4)) {
                        const float* pos = reinterpret_cast<const float*>(
                            reinterpret_cast<uintptr_t>(s) +
                            NIGHT_CREATURE_POSITION_OFFSET);
                        if (std::isfinite(pos[0]) && std::isfinite(pos[1])) {
                            px = pos[0];
                            py = pos[1];
                            posOk = true;
                        }
                    }

                    // v1.0.6: Dump +0x250~+0x290 region to find dataID
                    // fields in live gimmick objects.  Log first 5 objects
                    // with non-zero values in this range.
                    {
                        static std::atomic<int> s_dumpCount{0};
                        int di = s_dumpCount.fetch_add(1,
                            std::memory_order_relaxed);
                        if (di < 5 && bid != 0) {
                            Log("[NightMapMarkers][diag] LIVE+0x250~0x290 "
                                "baseID=%llu pos=(%.1f,%.1f):\n",
                                static_cast<unsigned long long>(bid),
                                px, py);
                            for (uintptr_t off = 0x250; off <= 0x290;
                                 off += 8) {
                                if (!IsReadable(s, off + sizeof(u64)))
                                    break;
                                u64 val = *reinterpret_cast<const u64*>(
                                    reinterpret_cast<uintptr_t>(s) + off);
                                if (val != 0)
                                    Log("[NightMapMarkers][diag]   +0x%03llX "
                                        "= %llu (0x%llX)\n",
                                        static_cast<unsigned long long>(off),
                                        static_cast<unsigned long long>(val),
                                        static_cast<unsigned long long>(val));
                            }
                        }
                    }

                    if (bid == 0) {
                        ++zeroBaseIdCount;
                        continue;
                    }
                    ++nonZeroBaseIdCount;

                    // Find or insert into freqTable
                    size_t found = freqUsed;
                    for (size_t fi = 0; fi < freqUsed; ++fi) {
                        if (freqTable[fi].baseId == bid) { found = fi; break; }
                    }
                    if (found == freqUsed) {
                        if (freqUsed < 128) {
                            freqTable[freqUsed].baseId = bid;
                            freqTable[freqUsed].count = 1;
                            freqTable[freqUsed].firstItemNum = itemNum;
                            freqTable[freqUsed].firstX = px;
                            freqTable[freqUsed].firstY = py;
                            ++freqUsed;
                        }
                    } else {
                        ++freqTable[found].count;
                    }
                }

                Log("[NightMapMarkers][diag] FREQ cb=0x%X total=%zu "
                    "nonZero=%zu zero=%zu distinct=%zu\n",
                    vtableRva, resultCount, nonZeroBaseIdCount,
                    zeroBaseIdCount, freqUsed);

                for (size_t fi = 0; fi < freqUsed; ++fi) {
                    Log("[NightMapMarkers][diag] FREQ cb=0x%X baseID=%llu "
                        "count=%u itemNum=%d pos=(%.1f,%.1f)\n",
                        vtableRva,
                        static_cast<unsigned long long>(freqTable[fi].baseId),
                        freqTable[fi].count,
                        freqTable[fi].firstItemNum,
                        freqTable[fi].firstX, freqTable[fi].firstY);
                }
            } else {
                // Default: log first NIGHT_DIAG_OBJECTS_PER_CALLBACK objects
                const size_t limit = resultCount < NIGHT_DIAG_OBJECTS_PER_CALLBACK
                    ? resultCount : NIGHT_DIAG_OBJECTS_PER_CALLBACK;
                for (size_t oi = 0; oi < limit; ++oi) {
                    if (objectLines >= NIGHT_DIAG_MAX_OBJECT_LINES) break;
                    NightDiagLogObject(vtableRva, oi, results.begin[oi], base);
                    ++objectLines;
                }
                // vtable cluster summary (first 64)
                const size_t clusterLimit = resultCount < 64 ? resultCount : 64;
                uint32_t clusterVtables[16] = {};
                uint32_t clusterCounts[16] = {};
                size_t clusterUsed = 0;
                for (size_t oi = 0; oi < clusterLimit; ++oi) {
                    void* s = results.begin[oi];
                    if (!s || !IsReadable(s, sizeof(void*))) continue;
                    void* sv = *reinterpret_cast<void**>(s);
                    if (!sv) continue;
                    const uint32_t srva = static_cast<uint32_t>(
                        reinterpret_cast<uintptr_t>(sv) - base);
                    size_t found = clusterUsed;
                    for (size_t ci2 = 0; ci2 < clusterUsed; ++ci2) {
                        if (clusterVtables[ci2] == srva) { found = ci2; break; }
                    }
                    if (found == clusterUsed) {
                        if (clusterUsed < 16) {
                            clusterVtables[clusterUsed] = srva;
                            clusterCounts[clusterUsed] = 1;
                            ++clusterUsed;
                        }
                    } else {
                        ++clusterCounts[found];
                    }
                }
                for (size_t ci2 = 0; ci2 < clusterUsed; ++ci2) {
                    Log("[NightMapMarkers][diag] vcluster cb=0x%X vtable=0x%X "
                        "count=%zu (first64)\n",
                        vtableRva, clusterVtables[ci2],
                        static_cast<size_t>(clusterCounts[ci2]));
                }
            }
        }
        NightReleaseSearchVector(&results);
        if (objectLines >= NIGHT_DIAG_MAX_OBJECT_LINES) {
            Log("[NightMapMarkers][diag] object line cap reached; abort early\n");
            break;
        }
    }
    Log("[NightMapMarkers][diag] callback_scan complete object_lines=%zu\n",
        objectLines);
}

// ===========================================================================
// NightDiagEnumerateHashTable (F7) - enumerate g_gimmickMgr hash table
// ===========================================================================
//
// Replaces the failed 0x250910 direct-call probe (all SEH due to missing
// TLS context object).  Instead of calling any game function, we directly
// traverse the hash table data structure in g_gimmickMgr.
//
// Hash table structure (from capstone disasm of 0x17C480 + 0x3F9E0):
//   g_gimmickMgr = [base + 0x10DCA20]
//   Container starts at mgr + 0x58:
//     [+0x08] = sentinel node (circular doubly-linked list head)
//     [+0x18] = bucket array pointer
//     [+0x30] = mask (bucket count - 1)
//   Node layout (intrusive linked list + hash entry):
//     [+0x00] = prev pointer
//     [+0x08] = next pointer
//     [+0x10] = key (u64, used as rdx in 0x250910 calls)
//     [+0x18] = value pointer (gimmick status object)
//
// Traversal: follow sentinel->next chain until we loop back to sentinel.
// This enumerates ALL live gimmicks without calling any game code.

static void NightDiagEnumerateHashTable() {
    const uintptr_t base = reinterpret_cast<uintptr_t>(
        GetModuleHandleW(nullptr));

    Log("[NightMapMarkers][hash] === g_gimmickMgr hash table enum ===\n");

    // --- 1. Read g_gimmickMgr ---
    void* mgr = nullptr;
    if (IsReadable(reinterpret_cast<void*>(base + RVA_NIGHT_GIMMICK_MGR),
            sizeof(void*))) {
        mgr = *reinterpret_cast<void**>(
            base + RVA_NIGHT_GIMMICK_MGR);
    }
    if (!mgr) {
        Log("[NightMapMarkers][hash] g_gimmickMgr is null\n");
        return;
    }
    Log("[NightMapMarkers][hash] g_gimmickMgr=%p\n", mgr);

    // --- 2. Read container fields (mgr + 0x58) ---
    uintptr_t mgrAddr = reinterpret_cast<uintptr_t>(mgr);
    uintptr_t containerAddr = mgrAddr + 0x58;

    void* sentinel = nullptr;
    void* bucketArray = nullptr;
    u64 mask = 0;

    if (!IsReadable(reinterpret_cast<void*>(containerAddr), 0x38)) {
        Log("[NightMapMarkers][hash] container not readable at %p\n",
            reinterpret_cast<void*>(containerAddr));
        return;
    }

    sentinel = *reinterpret_cast<void**>(containerAddr + 0x08);
    bucketArray = *reinterpret_cast<void**>(containerAddr + 0x18);
    mask = *reinterpret_cast<u64*>(containerAddr + 0x30);

    Log("[NightMapMarkers][hash] sentinel=%p bucketArray=%p "
        "mask=0x%llX (buckets=%llu)\n",
        sentinel, bucketArray,
        static_cast<unsigned long long>(mask),
        static_cast<unsigned long long>(mask + 1));

    if (!sentinel) {
        Log("[NightMapMarkers][hash] sentinel is null\n");
        return;
    }

    // --- 3. Traverse intrusive circular list from sentinel->next ---
    // sentinel+0x08 = next (first real node)
    void* firstNode = nullptr;
    if (IsReadable(reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(sentinel) + 0x08), sizeof(void*))) {
        firstNode = *reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(sentinel) + 0x08);
    }
    Log("[NightMapMarkers][hash] sentinel->next=%p\n", firstNode);

    if (!firstNode) {
        Log("[NightMapMarkers][hash] firstNode is null\n");
        return;
    }

    // --- 4. Enumerate all nodes ---
    int totalEntries = 0;
    int loggedEntries = 0;
    const int MAX_LOG = 300;
    const int MAX_TOTAL = 5000;

    void* node = firstNode;
    while (node && node != sentinel && totalEntries < MAX_TOTAL) {
        // Check readability of node header
        if (!IsReadable(node, 0x20)) {
            Log("[NightMapMarkers][hash] node %p not readable, stopping\n",
                node);
            break;
        }

        // Read key (node+0x10) and value (node+0x18)
        u64 key = *reinterpret_cast<u64*>(
            reinterpret_cast<uintptr_t>(node) + 0x10);
        void* value = *reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(node) + 0x18);

        // Try to read identifying info from the value (gimmick status)
        float px = 0.0f, py = 0.0f, pz = 0.0f;
        u64 bid = 0;
        int32_t itemNum = -1;
        int32_t enabled = -1;
        void* vtable = nullptr;

        if (value && IsReadable(value, 0x08)) {
            vtable = *reinterpret_cast<void**>(value);
        }

        if (value && IsReadable(value, 0xF0 + sizeof(float) * 3)) {
            const float* pos = reinterpret_cast<const float*>(
                reinterpret_cast<uintptr_t>(value) + 0xF0);
            px = pos[0]; py = pos[1]; pz = pos[2];
        }

        // baseID via +0x240 -> holder -> data -> u64
        if (value && IsReadable(value, 0x248)) {
            void* holder = *reinterpret_cast<void**>(
                reinterpret_cast<uintptr_t>(value) + 0x240);
            if (holder && IsReadable(holder, sizeof(void*))) {
                void* data = *reinterpret_cast<void**>(holder);
                if (data && IsReadable(data, sizeof(u64))) {
                    bid = *reinterpret_cast<const u64*>(data);
                }
            }
        }

        // itemNum at +0x288
        if (value && IsReadable(value, 0x288 + sizeof(int32_t))) {
            itemNum = *reinterpret_cast<const int32_t*>(
                reinterpret_cast<uintptr_t>(value) + 0x288);
        }

        // enabled at +0x130
        if (value && IsReadable(value, 0x130 + sizeof(int32_t))) {
            enabled = *reinterpret_cast<const int32_t*>(
                reinterpret_cast<uintptr_t>(value) + 0x130);
        }

        // Log first MAX_LOG entries
        if (loggedEntries < MAX_LOG) {
            Log("[NightMapMarkers][hash] [%d] key=%llu bid=%llu "
                "itemNum=%d enabled=%d pos=(%.1f,%.1f,%.1f) "
                "vt=%p val=%p\n",
                totalEntries,
                static_cast<unsigned long long>(key),
                static_cast<unsigned long long>(bid),
                itemNum, enabled, px, py, pz, vtable, value);
            loggedEntries++;
        }

        totalEntries++;

        // Follow next pointer (node+0x08)
        void* next = *reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(node) + 0x08);
        if (next == node) {
            // Self-loop (shouldn't happen for real nodes)
            Log("[NightMapMarkers][hash] self-loop at node %p, stopping\n",
                node);
            break;
        }
        node = next;
    }

    Log("[NightMapMarkers][hash] total=%d logged=%d (cap=%d)\n",
        totalEntries, loggedEntries, MAX_TOTAL);

    // --- 5. Summary: group by baseID ---
    // (We already logged all entries above; user can analyze from log)
    Log("[NightMapMarkers][hash] === enumeration complete ===\n");
}


// ---- HotConfig 注册函数（在 .inl 中实现以访问 static 变量）----
void NightHotConfigRegisterAll() {
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_REDRAW, "RVA_NIGHT_MAP_REDRAW", HOT_RVA, 0x2F52F0);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_INITIALIZER_CALL, "RVA_NIGHT_MAP_INITIALIZER_CALL", HOT_RVA, 0x2F14F5);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_VECTOR_READ, "RVA_NIGHT_MAP_VECTOR_READ", HOT_RVA, 0x2F535C);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_VECTOR_RANGE_LOAD, "RVA_NIGHT_MAP_VECTOR_RANGE_LOAD", HOT_RVA, 0x2F536A);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_TASK_VTABLE, "RVA_NIGHT_MAP_TASK_VTABLE", HOT_RVA, 0xE42D38);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_TASK_COL, "RVA_NIGHT_MAP_TASK_COL", HOT_RVA, 0xEDDB88);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_TASK_TYPE, "RVA_NIGHT_MAP_TASK_TYPE", HOT_RVA, 0x1095748);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_TASK_INITIALIZE, "RVA_NIGHT_MAP_TASK_INITIALIZE", HOT_RVA, 0x2FE900);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_STAMP_VTABLE, "RVA_NIGHT_MAP_STAMP_VTABLE", HOT_RVA, 0xE2A368);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_STAMP_COL, "RVA_NIGHT_MAP_STAMP_COL", HOT_RVA, 0xEB67B0);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_STAMP_TYPE, "RVA_NIGHT_MAP_STAMP_TYPE", HOT_RVA, 0x1071260);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_STAMP_DESTROY, "RVA_NIGHT_MAP_STAMP_DESTROY", HOT_RVA, 0x11B710);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_CREATURE_VTABLE, "RVA_NIGHT_CREATURE_VTABLE", HOT_RVA, 0xE27AF8);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_CREATURE_COL, "RVA_NIGHT_CREATURE_COL", HOT_RVA, 0xEB1780);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_CREATURE_TYPE, "RVA_NIGHT_CREATURE_TYPE", HOT_RVA, 0x106D5A0);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_CREATURE_DESTROY, "RVA_NIGHT_CREATURE_DESTROY", HOT_RVA, 0xECA14);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_CREATURE_CALLBACK_VTABLE, "RVA_NIGHT_CREATURE_CALLBACK_VTABLE", HOT_RVA, 0xE3E5D0);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_CREATURE_CALLBACK_COL, "RVA_NIGHT_CREATURE_CALLBACK_COL", HOT_RVA, 0xED5A90);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_CREATURE_CALLBACK_COPY, "RVA_NIGHT_CREATURE_CALLBACK_COPY", HOT_RVA, 0x28E4C0);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_CREATURE_CALLBACK_INVOKE, "RVA_NIGHT_CREATURE_CALLBACK_INVOKE", HOT_RVA, 0x28E4B0);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_CREATURE_CALLBACK_DESTROY, "RVA_NIGHT_CREATURE_CALLBACK_DESTROY", HOT_RVA, 0x1969B0);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_INFORMATION_VTABLE, "RVA_NIGHT_MAP_INFORMATION_VTABLE", HOT_RVA, 0xE354E8);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_INFORMATION_COL, "RVA_NIGHT_MAP_INFORMATION_COL", HOT_RVA, 0xEC8330);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_INFORMATION_TYPE, "RVA_NIGHT_MAP_INFORMATION_TYPE", HOT_RVA, 0x1080890);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_COM_MAP_VTABLE, "RVA_NIGHT_COM_MAP_VTABLE", HOT_RVA, 0xE314D0);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_COM_MAP_COL, "RVA_NIGHT_COM_MAP_COL", HOT_RVA, 0xEC2228);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_COM_MAP_TYPE, "RVA_NIGHT_COM_MAP_TYPE", HOT_RVA, 0x107C640);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_COM_MAP_SECONDARY_10, "RVA_NIGHT_COM_MAP_SECONDARY_10", HOT_RVA, 0xE313B8);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_COM_MAP_SECONDARY_20, "RVA_NIGHT_COM_MAP_SECONDARY_20", HOT_RVA, 0xE314E0);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_COM_MAP_SECONDARY_248, "RVA_NIGHT_COM_MAP_SECONDARY_248", HOT_RVA, 0xE313C8);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_CREATURE_CONSTRUCTOR, "RVA_NIGHT_CREATURE_CONSTRUCTOR", HOT_RVA, 0xE7400);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_CREATURE_DATA_BIND, "RVA_NIGHT_CREATURE_DATA_BIND", HOT_RVA, 0xE779E);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_CREATURE_ID_ACCESS, "RVA_NIGHT_CREATURE_ID_ACCESS", HOT_RVA, 0xE80EC);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_CREATURE_ID_FORMAT, "RVA_NIGHT_CREATURE_ID_FORMAT", HOT_RVA, 0xE27588);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_SPATIAL_SEARCH, "RVA_NIGHT_SPATIAL_SEARCH", HOT_RVA, 0x194060);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_RAW_VECTOR_FREE, "RVA_NIGHT_RAW_VECTOR_FREE", HOT_RVA, 0xB8720);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_STATUS_UNIQUE_ID_GETTER, "RVA_NIGHT_STATUS_UNIQUE_ID_GETTER", HOT_RVA, 0x119590);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_STATUS_POSITION_GETTER, "RVA_NIGHT_STATUS_POSITION_GETTER", HOT_RVA, 0x1195A0);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_AREA_RESOLVER, "RVA_NIGHT_MAP_AREA_RESOLVER", HOT_RVA, 0x16D860);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_AREA_RESOLVER_BOUNDS, "RVA_NIGHT_MAP_AREA_RESOLVER_BOUNDS", HOT_RVA, 0x16D983);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_LIVE_AREA_RESOLVER, "RVA_NIGHT_LIVE_AREA_RESOLVER", HOT_RVA, 0x1D5F00);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_AREA_PLAYER_CHAIN, "RVA_NIGHT_MAP_AREA_PLAYER_CHAIN", HOT_RVA, 0x1725A1);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_AREA_PLAYER_LIVE_CALL, "RVA_NIGHT_MAP_AREA_PLAYER_LIVE_CALL", HOT_RVA, 0x1725D6);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_AREA_PLAYER_CALL, "RVA_NIGHT_MAP_AREA_PLAYER_CALL", HOT_RVA, 0x1725EB);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_AREA_STATUS_CHAIN, "RVA_NIGHT_MAP_AREA_STATUS_CHAIN", HOT_RVA, 0xE09C4);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_AREA_STATUS_LIVE_CALL, "RVA_NIGHT_MAP_AREA_STATUS_LIVE_CALL", HOT_RVA, 0xE09EB);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_AREA_STATUS_CALL, "RVA_NIGHT_MAP_AREA_STATUS_CALL", HOT_RVA, 0xE0A00);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_WORLD_TO_MAP, "RVA_NIGHT_WORLD_TO_MAP", HOT_RVA, 0x223600);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_VECTOR_ADD, "RVA_NIGHT_VECTOR_ADD", HOT_RVA, 0xE7250);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_WORLD_TO_MAP_CALL, "RVA_NIGHT_WORLD_TO_MAP_CALL", HOT_RVA, 0x2E9E5B);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_POSITION_GETTER_CALL, "RVA_NIGHT_POSITION_GETTER_CALL", HOT_RVA, 0x2E9E45);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_VECTOR_ADD_CALL, "RVA_NIGHT_VECTOR_ADD_CALL", HOT_RVA, 0x2E9EAD);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_STAMP_POSITION_COPY, "RVA_NIGHT_MAP_STAMP_POSITION_COPY", HOT_RVA, 0x2F5526);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_STAMP_NATIVE_LIMIT, "RVA_NIGHT_MAP_STAMP_NATIVE_LIMIT", HOT_RVA, 0x2F5512);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_STAMP_ALLOCATION, "RVA_NIGHT_MAP_STAMP_ALLOCATION", HOT_RVA, 0x2F5540);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_STAMP_LAYOUT_WRITES, "RVA_NIGHT_MAP_STAMP_LAYOUT_WRITES", HOT_RVA, 0x2F557F);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_STAMP_ATLAS_TYPE, "RVA_NIGHT_MAP_STAMP_ATLAS_TYPE", HOT_RVA, 0x2F5394);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_STAMP_SELECTION, "RVA_NIGHT_MAP_STAMP_SELECTION", HOT_RVA, 0x2F77C2);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_STAMP_POSITION_READ, "RVA_NIGHT_STAMP_POSITION_READ", HOT_RVA, 0x2F5406);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_STAMP_RENDER_ARGS, "RVA_NIGHT_STAMP_RENDER_ARGS", HOT_RVA, 0x2F544E);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_STAMP_RENDER_CALL, "RVA_NIGHT_STAMP_RENDER_CALL", HOT_RVA, 0x2F5474);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_STAMP_LOOP_ADVANCE, "RVA_NIGHT_STAMP_LOOP_ADVANCE", HOT_RVA, 0x2F5479);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_STAMP_RENDERER, "RVA_NIGHT_STAMP_RENDERER", HOT_RVA, 0x740C50);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_RENDERER_R9_OVERWRITE, "RVA_NIGHT_RENDERER_R9_OVERWRITE", HOT_RVA, 0x740CD2);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_ENABLED_MEMBERSHIP_PROOF, "RVA_NIGHT_ENABLED_MEMBERSHIP_PROOF", HOT_RVA, 0x1916D2);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_SPATIAL_OWNER_CHAIN, "RVA_NIGHT_SPATIAL_OWNER_CHAIN", HOT_RVA, 0xE05D3);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_SPATIAL_OWNER_LOOKUP_CALL, "RVA_NIGHT_SPATIAL_OWNER_LOOKUP_CALL", HOT_RVA, 0xE05EF);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_SPATIAL_OWNER_LOOKUP, "RVA_NIGHT_SPATIAL_OWNER_LOOKUP", HOT_RVA, 0x18E0C0);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_SPATIAL_ADMISSION_WINDOW, "RVA_NIGHT_SPATIAL_ADMISSION_WINDOW", HOT_RVA, 0xEC576);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_SPATIAL_ADMISSION_CALL, "RVA_NIGHT_SPATIAL_ADMISSION_CALL", HOT_RVA, 0xEC580);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_SPATIAL_ADMISSION, "RVA_NIGHT_SPATIAL_ADMISSION", HOT_RVA, 0x18DE80);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_SPATIAL_INDEX_INSERT_WINDOW, "RVA_NIGHT_SPATIAL_INDEX_INSERT_WINDOW", HOT_RVA, 0x18DEDD);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_SPATIAL_INDEX_INSERT_CALL, "RVA_NIGHT_SPATIAL_INDEX_INSERT_CALL", HOT_RVA, 0x18DEEF);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_SPATIAL_INDEX_INSERT, "RVA_NIGHT_SPATIAL_INDEX_INSERT", HOT_RVA, 0x11A880);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MEMBERSHIP_ADMISSION_CALL, "RVA_NIGHT_MEMBERSHIP_ADMISSION_CALL", HOT_RVA, 0x191722);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_DIVISOR, "RVA_NIGHT_MAP_DIVISOR", HOT_RVA, 0xA2AA88);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_SCALE_ONE, "RVA_NIGHT_MAP_SCALE_ONE", HOT_RVA, 0xE92D54);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_OFFSET_X, "RVA_NIGHT_MAP_OFFSET_X", HOT_RVA, 0xE932B0);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_OFFSET_Y, "RVA_NIGHT_MAP_OFFSET_Y", HOT_RVA, 0xE93544);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_PLAYER_STATUS_VTABLE, "RVA_NIGHT_PLAYER_STATUS_VTABLE", HOT_RVA, 0xE2B790);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_PLAYER_STATUS_COL, "RVA_NIGHT_PLAYER_STATUS_COL", HOT_RVA, 0xEB8AC0);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_PLAYER_STATUS_TYPE, "RVA_NIGHT_PLAYER_STATUS_TYPE", HOT_RVA, 0x10730C0);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_GIMMICK_MGR, "RVA_NIGHT_GIMMICK_MGR", HOT_RVA, 0x1104C80);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_STATUS_SEARCH_BY_BASE_ID, "RVA_NIGHT_STATUS_SEARCH_BY_BASE_ID", HOT_RVA, 0x259300);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_EXACT_ANCHOR_SEARCH, "RVA_NIGHT_EXACT_ANCHOR_SEARCH", HOT_RVA, 0x1CC450);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_MAP_HASH_LOOKUP, "RVA_NIGHT_MAP_HASH_LOOKUP", HOT_RVA, 0x18E0C0);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_SAVE_DATA_ACCESSOR, "RVA_NIGHT_SAVE_DATA_ACCESSOR", HOT_RVA, 0x27A40);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_SAVE_DATA_PTR, "RVA_NIGHT_SAVE_DATA_PTR", HOT_RVA, 0x10FCBB0);
    HotConfig_Register("monstermark", (void*)&RVA_NIGHT_GAME_DB_PTR, "RVA_NIGHT_GAME_DB_PTR", HOT_RVA, 0x1104C10);
}
