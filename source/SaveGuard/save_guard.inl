// ---- save integrity patrol -----------------------------------------------
//
// SaveGuard v1.0.0 巡检实现（由 saveguard.cpp 尾部 #include 引入）。
//
// 原则：绝不写入 live 存档目录。只在存档目录同级（AppData 的
// Honogurashinoniwa\SaveGuard_backup\<SteamID64>\）发布自己拥有的快照。
//
// 备份目录布局：
//   %APPDATA%\Nippon Ichi Software, Inc\Honogurashinoniwa\SaveGuard_backup\
//       README_如何恢复存档.txt
//       <SteamID64>\snapshot_YYYYMMDD_HHMMSS_SEQ\   最近 8 份健康快照
//       <SteamID64>\damaged_YYYYMMDD_HHMMSS_SEQ\    最近 8 份损坏现场
//
// 巡检状态机（mod_tick 驱动，节流轮询）：
//   枚举存档目录 → 与上次观察比对（name/size/lastWrite）
//   变化      → 重置稳定计时（等待写盘结束）
//   稳定 2s   → 逐文件 YKCMP 容器校验
//   全部通过  → 重发布健康快照（partial→哈希双检→manifest→原子改名）
//   出现损坏  → 记录首次失败，3s 后重测；连续两次失败定性损坏
//               → LogE 告警 + 抢救全部文件到 damaged_ 快照 + report.txt
//
// 容器头（FN-009，2026-10-12 实测验证）：
//   0x14B = magic(8B)="YKCMP_V1" | ver(4B) | A(4B) | B(4B)，流起点 0x14
//   ver=4  → save.lst（实测 size=644, A=644, B=3320；A 语义与 ver8 不同，
//            不做 0x14+A<=size 校验）
//   ver=8  → save.NNN（3MB）/ .systemsave（512KB）
//            （实测 A=81K~445K，B=20971520 固定 20MB 解压缓冲）
//
// 该模块不依赖任何游戏 RVA / hook：校验全部基于磁盘文件字节。

static constexpr size_t SAVE_GUARD_PATH_CAP = 2048;
static constexpr size_t SAVE_GUARD_MAX_FILES = 102;
static constexpr size_t SAVE_GUARD_MAX_SNAPSHOTS = 8;
static constexpr size_t SAVE_GUARD_MAX_DAMAGED = 8;
static constexpr ULONGLONG SAVE_GUARD_POLL_MS = 500;
static constexpr ULONGLONG SAVE_GUARD_STABLE_MS = 2000;
static constexpr ULONGLONG SAVE_GUARD_RETEST_MS = 3000;
static constexpr ULONGLONG SAVE_GUARD_RETRY_MS = 1000;
static constexpr ULONGLONG SAVE_GUARD_ACCOUNT_RETRY_MS = 2000;
static constexpr ULONGLONG SAVE_GUARD_MAX_FILE_SIZE = 64ULL * 1024ULL * 1024ULL;
static constexpr ULONGLONG SAVE_GUARD_MAX_TOTAL_SIZE = 512ULL * 1024ULL * 1024ULL;

// 固定布局（实测：槽位 3MB / .systemsave 512KB）
static constexpr unsigned long long SAVE_GUARD_SLOT_FILE_SIZE = 3145728ULL;
static constexpr unsigned long long SAVE_GUARD_SYSTEM_FILE_SIZE = 524288ULL;
// save.lst 大小随槽位内容变化（实测 644B），只设宽松上下限
static constexpr unsigned long long SAVE_GUARD_LIST_MIN_SIZE = 0x14ULL;
static constexpr unsigned long long SAVE_GUARD_LIST_MAX_SIZE = 1048576ULL;
static constexpr unsigned SAVE_GUARD_HEADER_SIZE = 0x14;
static constexpr unsigned SAVE_GUARD_VER_LIST = 4;
static constexpr unsigned SAVE_GUARD_VER_SLOT = 8;
static constexpr unsigned SAVE_GUARD_VER_SYSTEM = 8;
// B 字段=解压缓冲上限：槽位实测 20MB；上限放宽到 64MB 防荒谬值
static constexpr unsigned long long SAVE_GUARD_EXPAND_MAX = 64ULL * 1024ULL * 1024ULL;
static constexpr u64 SAVE_GUARD_STEAM_ID64_BASE = 76561197960265728ULL;

static constexpr wchar_t SAVE_GUARD_BACKUP_DIR_NAME[] = L"SaveGuard_backup";
static constexpr wchar_t SAVE_GUARD_FINAL_MARKER_NAME[] = L".saveguard_final";
static constexpr wchar_t SAVE_GUARD_PARTIAL_MARKER_NAME[] = L".saveguard_partial";
static constexpr wchar_t SAVE_GUARD_SNAPSHOT_PREFIX[] = L"snapshot_";
static constexpr wchar_t SAVE_GUARD_DAMAGED_PREFIX[] = L"damaged_";
static constexpr wchar_t SAVE_GUARD_PARTIAL_PREFIX[] = L".partial_";
static constexpr char SAVE_GUARD_FINAL_MARKER[] =
    "VILLAGE_SAVEGUARD_SNAPSHOT_FINAL_V1\n";
static constexpr char SAVE_GUARD_PARTIAL_MARKER[] =
    "VILLAGE_SAVEGUARD_SNAPSHOT_PARTIAL_V1\n";

// ============================================================
// 类型定义
// ============================================================

enum class SaveGuardFileKind : unsigned {
    Slot = 0,     // save.NNN
    System = 1,   // .systemsave
    List = 2,     // save.lst
    Unknown = 3,  // 白名单之外（如 save.008bk），不参与校验
};

enum class SaveGuardValidateCode : unsigned {
    Passed = 0,
    OpenFailed = 1,        // 打不开 / 是目录 / 被占用
    HeaderReadFailed = 2,  // 0x14 字节头都读不完整
    MagicMismatch = 3,     // 前 8 字节 != "YKCMP_V1"
    SizeMismatch = 4,      // 固定长度不符（槽位/系统档）
    VersionMismatch = 5,   // ver 字段与文件类别不符
    DataLengthBad = 6,     // A 字段非法（0 或压缩流出文件界，仅 ver8）
    ExpandLimitBad = 7,    // B 字段非法（B < A 或超 64MB 上限）
};

enum class SaveGuardPublishResult : unsigned {
    Failed = 0,
    SourceChanged = 1,
    Published = 2,
    Pending = 3,  // v1.1.3：后台线程执行中
};

struct SaveGuardFileIdentity {
    wchar_t name[16];
    ULONGLONG size;
    ULONGLONG lastWrite;
};

struct SaveGuardFileSet {
    SaveGuardFileIdentity files[SAVE_GUARD_MAX_FILES];
    size_t count;
    ULONGLONG totalSize;
};

struct SaveGuardHashIdentity {
    unsigned char sha256[32];
    unsigned size;
};

struct SaveGuardHashSet {
    SaveGuardHashIdentity files[SAVE_GUARD_MAX_FILES];
    size_t count;
};

struct SaveGuardFileFinding {
    wchar_t name[16];
    SaveGuardValidateCode code;
    unsigned ver;
    unsigned dataLength;
    unsigned expandLimit;
    unsigned long long size;
};

// ============================================================
// 全局状态（仅 mod_init / mod_tick 线程访问）
// ============================================================

static bool g_guardInitialized = false;
static bool g_guardSourceReady = false;
static bool g_guardSourceMissingLogged = false;
static ULONGLONG g_guardNextPollAt = 0;
static ULONGLONG g_guardStableSince = 0;
static ULONGLONG g_guardNextAccountRetryAt = 0;
static ULONGLONG g_guardFirstFailureAt = 0;
static volatile LONG g_guardSequence = 0;  // v1.1.3：后台线程递增 → 原子
static wchar_t g_guardLiveRoot[SAVE_GUARD_PATH_CAP] = {};
static wchar_t g_guardSteamId[32] = {};
static wchar_t g_guardBackupRoot[SAVE_GUARD_PATH_CAP] = {};
static wchar_t g_guardProfileRoot[SAVE_GUARD_PATH_CAP] = {};
static SaveGuardFileSet g_guardObserved = {};
static SaveGuardFileSet g_guardValidatedSet = {};
static bool g_guardValidatedFresh = false;
static SaveGuardFileSet g_guardDamagedSet = {};
static bool g_guardDamagedFresh = false;
static SaveGuardFileFinding g_guardFindings[SAVE_GUARD_MAX_FILES];
// v1.1.3：已记录非白名单文件名环形表（旧版单名字对比导致 "."/".."/"save.008bk"
// 轮流出现时每轮 poll 刷 3 行，一局游戏 261 行噪音）
static wchar_t g_guardUnknownLoggedNames[8][32] = {};
static size_t g_guardUnknownLoggedCount = 0;

// v1.1.3：快照后台发布（25.7MB 复制+双轮哈希从主线程移出，消除 2.6s 冻结）
static HANDLE g_guardPublishThread = nullptr;
static CRITICAL_SECTION g_guardPublishLock;
static CONDITION_VARIABLE g_guardPublishWake;
static bool g_guardPublishLockReady = false;
static volatile LONG g_guardPublishState = 0;  // 0=idle 1=busy 2=done待主线程应用
static SaveGuardFileSet g_guardPublishRequest = {};
static SaveGuardPublishResult g_guardPublishResult =
    SaveGuardPublishResult::Failed;
static SaveGuardFileSet g_guardPublishObserved = {};
static bool g_guardPublishObservedValid = false;

// ============================================================
// 基础工具（路径 / 目录 / 文件 I/O，复用 SaveBackup 验证过的实现）
// ============================================================

static ULONGLONG SaveGuardFileTimeValue(FILETIME value) {
    ULARGE_INTEGER converted = {};
    converted.LowPart = value.dwLowDateTime;
    converted.HighPart = value.dwHighDateTime;
    return converted.QuadPart;
}

static bool SaveGuardJoinPath(wchar_t* output, size_t capacity,
                              const wchar_t* left, const wchar_t* right) {
    if (!output || capacity == 0 || !left || !right || !left[0] || !right[0]) {
        return false;
    }
    const size_t leftLength = wcslen(left);
    const size_t rightLength = wcslen(right);
    const bool separatorNeeded = left[leftLength - 1] != L'\\' &&
                                 left[leftLength - 1] != L'/';
    if (leftLength + (separatorNeeded ? 1 : 0) + rightLength + 1 > capacity) {
        return false;
    }
    memcpy(output, left, leftLength * sizeof(wchar_t));
    size_t cursor = leftLength;
    if (separatorNeeded) output[cursor++] = L'\\';
    memcpy(output + cursor, right, (rightLength + 1) * sizeof(wchar_t));
    return true;
}

static bool SaveGuardDirectoryIsPlain(const wchar_t* path) {
    if (!path || !path[0]) return false;
    const DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

static bool SaveGuardEnsureDirectory(const wchar_t* path) {
    if (!path || !path[0]) return false;
    if (!CreateDirectoryW(path, nullptr)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    return SaveGuardDirectoryIsPlain(path);
}

static bool SaveGuardPathIsUnderProfile(const wchar_t* path) {
    if (!path || !path[0] || !g_guardProfileRoot[0]) return false;
    const size_t rootLength = wcslen(g_guardProfileRoot);
    return _wcsnicmp(path, g_guardProfileRoot, rootLength) == 0 &&
           path[rootLength] == L'\\' && path[rootLength + 1] != L'\0';
}

static bool SaveGuardWriteBytes(const wchar_t* path, const void* bytes,
                                DWORD byteCount) {
    if (!path || !bytes || byteCount == 0) return false;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes, byteCount, &written, nullptr) &&
                    written == byteCount && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) DeleteFileW(path);
    return ok;
}

static bool SaveGuardWriteUtf8(const wchar_t* path, const wchar_t* text) {
    if (!path || !text) return false;
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                          text, -1, nullptr, 0,
                                          nullptr, nullptr);
    if (needed <= 1 || needed > 30000) return false;
    char buffer[30003] = {};
    buffer[0] = static_cast<char>(0xef);
    buffer[1] = static_cast<char>(0xbb);
    buffer[2] = static_cast<char>(0xbf);
    const int converted = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, buffer + 3,
        static_cast<int>(sizeof(buffer) - 3), nullptr, nullptr);
    if (converted != needed) return false;
    return SaveGuardWriteBytes(
        path, buffer, static_cast<DWORD>(3 + converted - 1));
}

static bool SaveGuardReadSmallFile(const wchar_t* path, char* output,
                                   size_t capacity, DWORD* usedOut) {
    if (usedOut) *usedOut = 0;
    if (!path || !output || capacity < 2 || capacity > 0xffffffffULL) {
        return false;
    }
    HANDLE file = CreateFileW(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    const bool sizeOk = GetFileSizeEx(file, &size) && size.QuadPart >= 0 &&
                        static_cast<ULONGLONG>(size.QuadPart) < capacity;
    DWORD used = 0;
    const bool ok = sizeOk &&
        ReadFile(file, output, static_cast<DWORD>(size.QuadPart), &used,
                 nullptr) && used == static_cast<DWORD>(size.QuadPart);
    CloseHandle(file);
    if (!ok) return false;
    output[used] = '\0';
    if (usedOut) *usedOut = used;
    return true;
}

static bool SaveGuardMarkerEquals(const wchar_t* directory,
                                  const wchar_t* markerName,
                                  const char* expected) {
    wchar_t path[SAVE_GUARD_PATH_CAP] = {};
    if (!SaveGuardJoinPath(path, _countof(path), directory, markerName)) {
        return false;
    }
    char content[160] = {};
    DWORD used = 0;
    if (!SaveGuardReadSmallFile(path, content, sizeof(content), &used)) {
        return false;
    }
    const size_t expectedLength = strlen(expected);
    return used == expectedLength &&
           memcmp(content, expected, expectedLength) == 0;
}

static void SaveGuardSha256Text(const unsigned char digest[32],
                                char output[65]) {
    static constexpr char digits[] = "0123456789abcdef";
    for (size_t index = 0; index < 32; ++index) {
        output[index * 2] = digits[digest[index] >> 4];
        output[index * 2 + 1] = digits[digest[index] & 0x0f];
    }
    output[64] = '\0';
}

// ============================================================
// 存档文件名白名单（与游戏保存链路 FN-009 一致）
// ============================================================

static bool SaveGuardIsSlotFileName(const wchar_t* name) {
    // save.NNN：总长 8，NNN 为 000-999 数字
    if (wcslen(name) != 8 || _wcsnicmp(name, L"save.", 5) != 0) {
        return false;
    }
    return name[5] >= L'0' && name[5] <= L'9' &&
           name[6] >= L'0' && name[6] <= L'9' &&
           name[7] >= L'0' && name[7] <= L'9';
}

static SaveGuardFileKind SaveGuardClassifyFileName(const wchar_t* name) {
    if (!name || !name[0]) return SaveGuardFileKind::Unknown;
    if (_wcsicmp(name, L".systemsave") == 0) return SaveGuardFileKind::System;
    if (_wcsicmp(name, L"save.lst") == 0) return SaveGuardFileKind::List;
    if (SaveGuardIsSlotFileName(name)) return SaveGuardFileKind::Slot;
    return SaveGuardFileKind::Unknown;
}

static bool SaveGuardSnapshotEntryIsOwned(const wchar_t* name) {
    // 快照目录内只允许出现：存档组文件 + manifest/report + 自有 marker
    if (SaveGuardClassifyFileName(name) != SaveGuardFileKind::Unknown) {
        return true;
    }
    return _wcsicmp(name, L"manifest.txt") == 0 ||
           _wcsicmp(name, L"report.txt") == 0 ||
           _wcsicmp(name, SAVE_GUARD_FINAL_MARKER_NAME) == 0 ||
           _wcsicmp(name, SAVE_GUARD_PARTIAL_MARKER_NAME) == 0;
}

static bool SaveGuardFileSetEqual(const SaveGuardFileSet& left,
                                  const SaveGuardFileSet& right) {
    if (left.count != right.count || left.totalSize != right.totalSize) {
        return false;
    }
    for (size_t index = 0; index < left.count; ++index) {
        if (_wcsicmp(left.files[index].name,
                     right.files[index].name) != 0 ||
            left.files[index].size != right.files[index].size ||
            left.files[index].lastWrite != right.files[index].lastWrite) {
            return false;
        }
    }
    return true;
}

// v1.1.3：真存档组视图比较（save.NNN + save.lst；.systemsave 是高频系统档，
// 不参与快照触发与幂等短路——实测其独立变化每轮都触发 25.7MB 复制导致
// 主线程冻结，且刷 live_change 日志）
static bool SaveGuardSlotFilesEqual(const SaveGuardFileSet& left,
                                    const SaveGuardFileSet& right) {
    size_t li = 0, ri = 0;
    while (true) {
        while (li < left.count &&
               SaveGuardClassifyFileName(left.files[li].name) ==
                   SaveGuardFileKind::System) ++li;
        while (ri < right.count &&
               SaveGuardClassifyFileName(right.files[ri].name) ==
                   SaveGuardFileKind::System) ++ri;
        const bool leftDone = li >= left.count;
        const bool rightDone = ri >= right.count;
        if (leftDone || rightDone) return leftDone && rightDone;
        if (_wcsicmp(left.files[li].name, right.files[ri].name) != 0 ||
            left.files[li].size != right.files[ri].size ||
            left.files[li].lastWrite != right.files[ri].lastWrite) {
            return false;
        }
        ++li; ++ri;
    }
}

// 枚举 live 存档目录（只收集白名单文件；长度/内容的健康性留给校验阶段）。
// requirements：必须同时存在 .systemsave + save.lst + 至少一个槽位，
// 否则视为存档组不完整（返回 false，不算损坏——等待游戏首次写盘）。
static bool SaveGuardEnumerateLiveFiles(const wchar_t* directory,
                                        SaveGuardFileSet* output) {
    if (!directory || !output || !SaveGuardDirectoryIsPlain(directory)) {
        return false;
    }
    *output = {};
    wchar_t pattern[SAVE_GUARD_PATH_CAP] = {};
    if (!SaveGuardJoinPath(pattern, _countof(pattern), directory, L"*")) {
        return false;
    }
    WIN32_FIND_DATAW found = {};
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    bool hasSystem = false;
    bool hasList = false;
    bool hasSlot = false;
    do {
        const SaveGuardFileKind kind = SaveGuardClassifyFileName(
            found.cFileName);
        if (kind == SaveGuardFileKind::Unknown) {
            // 白名单之外（实测存在 save.008bk 之类游戏自留备份）：
            // 不参与校验；每个名字仅首次出现时记一条（v1.1.3 环形表防刷屏）
            bool known = false;
            for (size_t i = 0; i < g_guardUnknownLoggedCount; ++i) {
                if (_wcsicmp(g_guardUnknownLoggedNames[i], found.cFileName) == 0) {
                    known = true; break;
                }
            }
            if (!known) {
                Log("[SaveGuard] ignored_nonlive_file name=%ls "
                    "reason=NOT_IN_SAVE_SET\n", found.cFileName);
                if (g_guardUnknownLoggedCount < 8) {
                    wcscpy_s(g_guardUnknownLoggedNames[
                                 g_guardUnknownLoggedCount],
                             found.cFileName);
                    ++g_guardUnknownLoggedCount;
                }
            }
            continue;
        }
        // 保存组里绝不接受目录或重解析点（如 save.001 恰好是目录）：
        // 记录为 identity，留给校验阶段按 OpenFailed 定性。
        SaveGuardFileIdentity entry = {};
        if (wcslen(found.cFileName) >= _countof(entry.name) ||
            output->count >= SAVE_GUARD_MAX_FILES) {
            ok = false;
            break;
        }
        wcscpy_s(entry.name, found.cFileName);
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            entry.size = 0;
        } else {
            ULARGE_INTEGER size = {};
            size.LowPart = found.nFileSizeLow;
            size.HighPart = found.nFileSizeHigh;
            entry.size = size.QuadPart;
        }
        entry.lastWrite = SaveGuardFileTimeValue(found.ftLastWriteTime);
        size_t insert = output->count;
        while (insert > 0 &&
               _wcsicmp(output->files[insert - 1].name, entry.name) > 0) {
            output->files[insert] = output->files[insert - 1];
            --insert;
        }
        if ((insert > 0 &&
             _wcsicmp(output->files[insert - 1].name, entry.name) == 0) ||
            (insert < output->count &&
             _wcsicmp(output->files[insert].name, entry.name) == 0)) {
            ok = false;
            break;
        }
        output->files[insert] = entry;
        ++output->count;
        output->totalSize += entry.size;
        hasSystem = hasSystem || kind == SaveGuardFileKind::System;
        hasList = hasList || kind == SaveGuardFileKind::List;
        hasSlot = hasSlot || kind == SaveGuardFileKind::Slot;
    } while (FindNextFileW(search, &found));
    const DWORD finalError = GetLastError();
    FindClose(search);
    if (ok && finalError != ERROR_NO_MORE_FILES) ok = false;
    if (ok && (output->totalSize > SAVE_GUARD_MAX_TOTAL_SIZE)) {
        Log("[SaveGuard] source_too_large total=%llu\n",
            static_cast<unsigned long long>(output->totalSize));
        return false;
    }
    if (ok && !(hasSystem && hasList && hasSlot && output->count >= 3)) {
        Log("[SaveGuard] save_set_incomplete system=%d list=%d slot=%d\n",
            hasSystem ? 1 : 0, hasList ? 1 : 0, hasSlot ? 1 : 0);
        return false;
    }
    return ok;
}

// ============================================================
// YKCMP 容器校验（FN-009 + 2026-10-12 实测参数）
// ============================================================

static const char* SaveGuardValidateCodeName(SaveGuardValidateCode code) {
    switch (code) {
    case SaveGuardValidateCode::Passed: return "passed";
    case SaveGuardValidateCode::OpenFailed: return "open_failed";
    case SaveGuardValidateCode::HeaderReadFailed: return "header_read_failed";
    case SaveGuardValidateCode::MagicMismatch: return "magic_mismatch";
    case SaveGuardValidateCode::SizeMismatch: return "size_mismatch";
    case SaveGuardValidateCode::VersionMismatch: return "version_mismatch";
    case SaveGuardValidateCode::DataLengthBad: return "data_length_bad";
    case SaveGuardValidateCode::ExpandLimitBad: return "expand_limit_bad";
    default: return "unknown";
    }
}

// 校验单个存档文件：共享读句柄（绝不妨碍游戏的读写删），现场重读大小与头部。
static SaveGuardValidateCode SaveGuardValidateFile(
        const wchar_t* path, SaveGuardFileKind kind,
        unsigned* verOut, unsigned* dataLengthOut,
        unsigned* expandLimitOut, unsigned long long* sizeOut) {
    if (verOut) *verOut = 0;
    if (dataLengthOut) *dataLengthOut = 0;
    if (expandLimitOut) *expandLimitOut = 0;
    if (sizeOut) *sizeOut = 0;
    if (kind == SaveGuardFileKind::Unknown) {
        return SaveGuardValidateCode::OpenFailed;
    }
    HANDLE file = CreateFileW(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return SaveGuardValidateCode::OpenFailed;
    }
    // 统一用现场大小判定（枚举值可能已过期）
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        CloseHandle(file);
        return SaveGuardValidateCode::OpenFailed;
    }
    const unsigned long long actualSize =
        static_cast<unsigned long long>(size.QuadPart);
    if (sizeOut) *sizeOut = actualSize;

    // 1) 固定长度（存档格式固定布局：槽位 3MB / 系统档 512KB；
    //    save.lst 大小随内容浮动，只给宽松上下限）
    if (kind == SaveGuardFileKind::Slot) {
        if (actualSize != SAVE_GUARD_SLOT_FILE_SIZE) {
            CloseHandle(file);
            return SaveGuardValidateCode::SizeMismatch;
        }
    } else if (kind == SaveGuardFileKind::System) {
        if (actualSize != SAVE_GUARD_SYSTEM_FILE_SIZE) {
            CloseHandle(file);
            return SaveGuardValidateCode::SizeMismatch;
        }
    } else if (kind == SaveGuardFileKind::List) {
        if (actualSize < SAVE_GUARD_LIST_MIN_SIZE ||
            actualSize > SAVE_GUARD_LIST_MAX_SIZE) {
            CloseHandle(file);
            return SaveGuardValidateCode::SizeMismatch;
        }
    }

    // 2) 容器头
    unsigned char header[SAVE_GUARD_HEADER_SIZE] = {};
    DWORD read = 0;
    if (!ReadFile(file, header, sizeof(header), &read, nullptr) ||
        read != sizeof(header)) {
        CloseHandle(file);
        return SaveGuardValidateCode::HeaderReadFailed;
    }
    unsigned ver = 0;
    unsigned dataLength = 0;
    unsigned expandLimit = 0;
    memcpy(&ver, header + 8, 4);
    memcpy(&dataLength, header + 12, 4);
    memcpy(&expandLimit, header + 16, 4);
    CloseHandle(file);
    if (verOut) *verOut = ver;
    if (dataLengthOut) *dataLengthOut = dataLength;
    if (expandLimitOut) *expandLimitOut = expandLimit;

    // 3) 魔数
    static constexpr char expectedMagic[] = "YKCMP_V1";
    if (memcmp(header, expectedMagic, 8) != 0) {
        return SaveGuardValidateCode::MagicMismatch;
    }

    // 4) ver 分发（0x8DFC50 跳表：save.lst→4，槽位/系统档→8）
    if (kind == SaveGuardFileKind::List) {
        if (ver != SAVE_GUARD_VER_LIST) return SaveGuardValidateCode::VersionMismatch;
    } else {
        if (ver != SAVE_GUARD_VER_SLOT) return SaveGuardValidateCode::VersionMismatch;
    }

    // 5) A 字段。注意 ver=4（save.lst）实测 A==size（语义与 ver=8 不同），
    //    只能按宽松规则；ver=8 实测 A 为压缩流长度，可校验流界。
    if (dataLength == 0) {
        return SaveGuardValidateCode::DataLengthBad;
    }
    if (kind != SaveGuardFileKind::List) {
        const unsigned long long headerSize = SAVE_GUARD_HEADER_SIZE;
        if (headerSize + dataLength > actualSize) {
            return SaveGuardValidateCode::DataLengthBad;
        }
    }

    // 6) B 字段：解压缓冲上限必须不小于数据长度，且有 64MB 荒谬上限
    if (expandLimit < dataLength ||
        expandLimit > SAVE_GUARD_EXPAND_MAX) {
        return SaveGuardValidateCode::ExpandLimitBad;
    }

    return SaveGuardValidateCode::Passed;
}

// 校验整组存档文件，结果写入 g_guardFindings（含失败原因与实测字段）。
// 返回 true=全部健康；false=至少一个文件异常。
static bool SaveGuardValidateSet(const wchar_t* directory,
                                 const SaveGuardFileSet& set) {
    bool allHealthy = true;
    for (size_t index = 0; index < set.count && index < SAVE_GUARD_MAX_FILES;
         ++index) {
        wchar_t path[SAVE_GUARD_PATH_CAP] = {};
        if (!SaveGuardJoinPath(path, _countof(path), directory,
                               set.files[index].name)) {
            allHealthy = false;
            wcscpy_s(g_guardFindings[index].name, set.files[index].name);
            g_guardFindings[index].code = SaveGuardValidateCode::OpenFailed;
            g_guardFindings[index].ver = 0;
            g_guardFindings[index].dataLength = 0;
            g_guardFindings[index].expandLimit = 0;
            g_guardFindings[index].size = 0;
            continue;
        }
        SaveGuardFileFinding finding = {};
        wcscpy_s(finding.name, set.files[index].name);
        finding.code = SaveGuardValidateFile(
            path, SaveGuardClassifyFileName(set.files[index].name),
            &finding.ver, &finding.dataLength, &finding.expandLimit,
            &finding.size);
        if (finding.code != SaveGuardValidateCode::Passed) allHealthy = false;
        g_guardFindings[index] = finding;
    }
    return allHealthy;
}

// ============================================================
// 哈希（用于快照的复制完整性双检 + manifest/report 记录）
// ============================================================

static bool SaveGuardHashFileSet(const wchar_t* directory,
                                 const SaveGuardFileSet& files,
                                 SaveGuardHashSet* output) {
    if (!directory || !output || files.count > SAVE_GUARD_MAX_FILES ||
        !SaveGuardDirectoryIsPlain(directory)) return false;
    *output = {};
    for (size_t index = 0; index < files.count; ++index) {
        wchar_t path[SAVE_GUARD_PATH_CAP] = {};
        if (!SaveGuardJoinPath(path, _countof(path), directory,
                               files.files[index].name)) return false;
        const DWORD attributes = GetFileAttributesW(path);
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
        DWORD error = ERROR_SUCCESS;
        unsigned size = 0;
        if (!PackageHashFile(path, output->files[index].sha256,
                             &size, &error) ||
            static_cast<ULONGLONG>(size) != files.files[index].size) {
            SetLastError(error != ERROR_SUCCESS ? error : ERROR_FILE_INVALID);
            return false;
        }
        output->files[index].size = size;
        ++output->count;
    }
    return output->count == files.count;
}

static bool SaveGuardHashSetEqual(const SaveGuardHashSet& left,
                                  const SaveGuardHashSet& right) {
    if (left.count != right.count) return false;
    for (size_t index = 0; index < left.count; ++index) {
        if (left.files[index].size != right.files[index].size ||
            memcmp(left.files[index].sha256,
                   right.files[index].sha256, 32) != 0) return false;
    }
    return true;
}

// ============================================================
// Steam 存档目录解析（复用 SaveBackup 的 ResolveLiveProfile 逻辑）
// ============================================================

static bool SaveGuardReadSteamAccountId(u64* accountIdOut) {
    if (!accountIdOut) return false;
    wchar_t cloudPath[SAVE_GUARD_PATH_CAP] = {};
    wchar_t parent[SAVE_GUARD_PATH_CAP] = {};
    const DWORD appDataLength = GetEnvironmentVariableW(
        L"APPDATA", parent, static_cast<DWORD>(_countof(parent)));
    if (!appDataLength || appDataLength >= _countof(parent)) return false;
    wchar_t productRoot[SAVE_GUARD_PATH_CAP] = {};
    if (!SaveGuardJoinPath(productRoot, _countof(productRoot), parent,
                           L"Nippon Ichi Software, Inc\\Honogurashinoniwa") ||
        !SaveGuardJoinPath(cloudPath, _countof(cloudPath), productRoot,
                           L"steam_autocloud.vdf")) return false;
    char content[4096] = {};
    DWORD used = 0;
    if (!SaveGuardReadSmallFile(cloudPath, content, sizeof(content), &used)) {
        return false;
    }
    static constexpr char key[] = "accountid";
    const char* begin = strstr(content, key);
    if (!begin) return false;
    begin += sizeof(key) - 1;
    while (*begin && (*begin < '0' || *begin > '9')) ++begin;
    if (!*begin) return false;
    u64 value = 0;
    unsigned digits = 0;
    while (*begin >= '0' && *begin <= '9') {
        const unsigned digit = static_cast<unsigned>(*begin - '0');
        if (value > (UINT64_MAX - digit) / 10ULL || ++digits > 10) {
            return false;
        }
        value = value * 10ULL + digit;
        ++begin;
    }
    if (digits == 0 || value > 0xffffffffULL) return false;
    *accountIdOut = value;
    return true;
}

static bool SaveGuardDigitsOnly(const wchar_t* text) {
    if (!text || !text[0]) return false;
    const size_t length = wcslen(text);
    if (length < 15 || length > 20) return false;
    for (size_t index = 0; index < length; ++index) {
        if (text[index] < L'0' || text[index] > L'9') return false;
    }
    return true;
}

static bool SaveGuardBuildProductRoot(wchar_t* output, size_t capacity) {
    wchar_t appData[SAVE_GUARD_PATH_CAP] = {};
    const DWORD length = GetEnvironmentVariableW(
        L"APPDATA", appData, static_cast<DWORD>(_countof(appData)));
    return length != 0 && length < _countof(appData) &&
        SaveGuardJoinPath(output, capacity, appData,
            L"Nippon Ichi Software, Inc\\Honogurashinoniwa");
}

// 解析 live 存档目录：优先用 steam_autocloud.vdf 的 accountid 精确命中，
// 失败则回退扫描纯数字目录取 .systemsave 最新的。
static bool SaveGuardResolveLiveProfile() {
    wchar_t productRoot[SAVE_GUARD_PATH_CAP] = {};
    if (!SaveGuardBuildProductRoot(productRoot, _countof(productRoot)) ||
        !SaveGuardDirectoryIsPlain(productRoot)) return false;

    wchar_t steamId[32] = {};
    u64 accountId = 0;
    if (SaveGuardReadSteamAccountId(&accountId) &&
        accountId <= UINT64_MAX - SAVE_GUARD_STEAM_ID64_BASE) {
        const u64 steamIdValue = SAVE_GUARD_STEAM_ID64_BASE + accountId;
        _snwprintf_s(steamId, _countof(steamId), _TRUNCATE,
                     L"%llu", static_cast<unsigned long long>(steamIdValue));
        wchar_t candidate[SAVE_GUARD_PATH_CAP] = {};
        if (SaveGuardJoinPath(candidate, _countof(candidate), productRoot,
                              steamId) &&
            SaveGuardDirectoryIsPlain(candidate)) {
            wchar_t systemPath[SAVE_GUARD_PATH_CAP] = {};
            if (SaveGuardJoinPath(systemPath, _countof(systemPath), candidate,
                                   L".systemsave") &&
                GetFileAttributesW(systemPath) != INVALID_FILE_ATTRIBUTES) {
                wcscpy_s(g_guardSteamId, steamId);
                wcscpy_s(g_guardLiveRoot, candidate);
                return true;
            }
        }
    }

    wchar_t pattern[SAVE_GUARD_PATH_CAP] = {};
    if (!SaveGuardJoinPath(pattern, _countof(pattern), productRoot, L"*")) {
        return false;
    }
    WIN32_FIND_DATAW found = {};
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return false;
    ULONGLONG newest = 0;
    wchar_t newestId[32] = {};
    wchar_t newestPath[SAVE_GUARD_PATH_CAP] = {};
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            !SaveGuardDigitsOnly(found.cFileName)) continue;
        wchar_t candidate[SAVE_GUARD_PATH_CAP] = {};
        wchar_t systemPath[SAVE_GUARD_PATH_CAP] = {};
        if (!SaveGuardJoinPath(candidate, _countof(candidate), productRoot,
                               found.cFileName) ||
            !SaveGuardJoinPath(systemPath, _countof(systemPath), candidate,
                               L".systemsave")) continue;
        WIN32_FILE_ATTRIBUTE_DATA info = {};
        if (!GetFileAttributesExW(systemPath, GetFileExInfoStandard, &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            continue;
        }
        const ULONGLONG written = SaveGuardFileTimeValue(info.ftLastWriteTime);
        if (!newestPath[0] || written > newest) {
            newest = written;
            wcscpy_s(newestId, found.cFileName);
            wcscpy_s(newestPath, candidate);
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
    if (!newestPath[0]) return false;
    wcscpy_s(g_guardSteamId, newestId);
    wcscpy_s(g_guardLiveRoot, newestPath);
    return true;
}

// ============================================================
// 快照目录管理（marker 验证 + 受控删除，防误删用户文件）
// ============================================================

static bool SaveGuardOwnedSnapshotMarker(const wchar_t* directory,
                                         bool allowPartial) {
    if (SaveGuardMarkerEquals(directory, SAVE_GUARD_FINAL_MARKER_NAME,
                              SAVE_GUARD_FINAL_MARKER)) return true;
    return allowPartial &&
        SaveGuardMarkerEquals(directory, SAVE_GUARD_PARTIAL_MARKER_NAME,
                              SAVE_GUARD_PARTIAL_MARKER);
}

static bool SaveGuardRemoveOwnedSnapshot(const wchar_t* directory,
                                         bool allowPartial) {
    if (!SaveGuardPathIsUnderProfile(directory) ||
        !SaveGuardDirectoryIsPlain(directory) ||
        !SaveGuardOwnedSnapshotMarker(directory, allowPartial)) return false;
    wchar_t pattern[SAVE_GUARD_PATH_CAP] = {};
    if (!SaveGuardJoinPath(pattern, _countof(pattern), directory, L"*")) {
        return false;
    }
    WIN32_FIND_DATAW found = {};
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return false;
    bool owned = true;
    do {
        if (_wcsicmp(found.cFileName, L".") == 0 ||
            _wcsicmp(found.cFileName, L"..") == 0) continue;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            !SaveGuardSnapshotEntryIsOwned(found.cFileName)) {
            owned = false;
            break;
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
    if (!owned) return false;

    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return false;
    bool deleted = true;
    do {
        if (_wcsicmp(found.cFileName, L".") == 0 ||
            _wcsicmp(found.cFileName, L"..") == 0) continue;
        wchar_t path[SAVE_GUARD_PATH_CAP] = {};
        if (!SaveGuardJoinPath(path, _countof(path), directory,
                               found.cFileName) || !DeleteFileW(path)) {
            deleted = false;
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return deleted && RemoveDirectoryW(directory) != FALSE;
}

// 轮换：清理残留 partial 半成品；按前缀保留最新 keepCount 份快照。
// 目录名自带时间戳+序号，字典序即时间序。
static void SaveGuardPruneSnapshots(const wchar_t* prefix, size_t keepCount) {
    if (!g_guardProfileRoot[0] ||
        !SaveGuardDirectoryIsPlain(g_guardProfileRoot)) return;
    const size_t prefixLength = wcslen(prefix);
    wchar_t names[SAVE_GUARD_MAX_SNAPSHOTS + SAVE_GUARD_MAX_DAMAGED + 16]
                 [64] = {};
    const size_t nameCap = _countof(names);
    size_t count = 0;
    wchar_t pattern[SAVE_GUARD_PATH_CAP] = {};
    if (!SaveGuardJoinPath(pattern, _countof(pattern), g_guardProfileRoot,
                           L"*")) return;
    WIN32_FIND_DATAW found = {};
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return;
    bool overflow = false;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            continue;
        }
        if (_wcsicmp(found.cFileName, L".") == 0 ||
            _wcsicmp(found.cFileName, L"..") == 0) continue;
        if (_wcsnicmp(found.cFileName, SAVE_GUARD_PARTIAL_PREFIX,
                      wcslen(SAVE_GUARD_PARTIAL_PREFIX)) == 0) {
            // 半成品（上次进程中断遗留）永远清理
            wchar_t child[SAVE_GUARD_PATH_CAP] = {};
            if (SaveGuardJoinPath(child, _countof(child), g_guardProfileRoot,
                                  found.cFileName)) {
                if (!SaveGuardRemoveOwnedSnapshot(child, true)) {
                    Log("[SaveGuard] cleanup_skip kind=partial path=%ls "
                        "reason=UNOWNED_OR_BUSY\n", child);
                }
            }
            continue;
        }
        if (_wcsnicmp(found.cFileName, prefix, prefixLength) != 0) continue;
        if (count >= nameCap) {
            overflow = true;
            break;
        }
        if (wcslen(found.cFileName) >= _countof(names[0])) continue;
        wcscpy_s(names[count], found.cFileName);
        ++count;
    } while (FindNextFileW(search, &found));
    FindClose(search);
    if (overflow) {
        Log("[SaveGuard] retention_skip reason=TOO_MANY_SNAPSHOTS prefix=%ls\n",
            prefix);
        return;
    }
    if (count <= keepCount) return;
    // 字典序排序（小→大=旧→新）
    for (size_t left = 0; left < count; ++left) {
        for (size_t right = left + 1; right < count; ++right) {
            if (_wcsicmp(names[right], names[left]) < 0) {
                wchar_t temporary[64] = {};
                wcscpy_s(temporary, names[left]);
                wcscpy_s(names[left], names[right]);
                wcscpy_s(names[right], temporary);
            }
        }
    }
    for (size_t index = 0; index + keepCount < count; ++index) {
        wchar_t child[SAVE_GUARD_PATH_CAP] = {};
        if (!SaveGuardJoinPath(child, _countof(child), g_guardProfileRoot,
                               names[index])) continue;
        if (SaveGuardRemoveOwnedSnapshot(child, false)) {
            Log("[SaveGuard] retention_removed path=%ls\n", names[index]);
        } else {
            Log("[SaveGuard] retention_skip path=%ls "
                "reason=UNOWNED_OR_BUSY\n",
                names[index]);
        }
    }
}

// ============================================================
// 复制（健康快照用 magic 双检版；损坏抢救用宽松版）
// ============================================================

static bool SaveGuardCopyOne(const wchar_t* sourceDirectory,
                             const wchar_t* targetDirectory,
                             const SaveGuardFileIdentity& identity) {
    wchar_t source[SAVE_GUARD_PATH_CAP] = {};
    wchar_t target[SAVE_GUARD_PATH_CAP] = {};
    if (!SaveGuardJoinPath(source, _countof(source), sourceDirectory,
                           identity.name) ||
        !SaveGuardJoinPath(target, _countof(target), targetDirectory,
                           identity.name)) return false;
    const DWORD sourceAttributes = GetFileAttributesW(source);
    if (sourceAttributes == INVALID_FILE_ATTRIBUTES ||
        (sourceAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (sourceAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;

    // 共享读句柄：绝不阻塞游戏的存档写盘 / 改名 / 删除
    HANDLE sourceFile = CreateFileW(
        source, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (sourceFile == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sourceSize = {};
    bool copied = GetFileSizeEx(sourceFile, &sourceSize) &&
        sourceSize.QuadPart >= 0 &&
        static_cast<ULONGLONG>(sourceSize.QuadPart) == identity.size;
    HANDLE targetFile = INVALID_HANDLE_VALUE;
    if (copied) {
        targetFile = CreateFileW(
            target, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        copied = targetFile != INVALID_HANDLE_VALUE;
    }
    unsigned char buffer[64 * 1024] = {};
    ULONGLONG remaining = identity.size;
    while (copied && remaining != 0) {
        const DWORD requested = static_cast<DWORD>(
            remaining < sizeof(buffer) ? remaining : sizeof(buffer));
        DWORD read = 0;
        DWORD written = 0;
        copied = ReadFile(sourceFile, buffer, requested, &read, nullptr) &&
                 read == requested &&
                 WriteFile(targetFile, buffer, read, &written, nullptr) &&
                 written == read;
        if (copied) remaining -= read;
    }
    SecureZeroMemory(buffer, sizeof(buffer));
    if (copied) copied = FlushFileBuffers(targetFile) != FALSE;
    if (targetFile != INVALID_HANDLE_VALUE) CloseHandle(targetFile);
    CloseHandle(sourceFile);
    if (!copied) return false;

    WIN32_FILE_ATTRIBUTE_DATA targetInfo = {};
    if (!GetFileAttributesExW(target, GetFileExInfoStandard, &targetInfo) ||
        (targetInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (targetInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
    }
    ULARGE_INTEGER size = {};
    size.LowPart = targetInfo.nFileSizeLow;
    size.HighPart = targetInfo.nFileSizeHigh;
    if (size.QuadPart != identity.size) return false;
    return true;
}

// 损坏取证用：损坏文件魔数必然不匹配，只校验大小与 Flush。
static bool SaveGuardCopyOneRaw(const wchar_t* sourceDirectory,
                                const wchar_t* targetDirectory,
                                const SaveGuardFileIdentity& identity) {
    wchar_t source[SAVE_GUARD_PATH_CAP] = {};
    wchar_t target[SAVE_GUARD_PATH_CAP] = {};
    if (!SaveGuardJoinPath(source, _countof(source), sourceDirectory,
                           identity.name) ||
        !SaveGuardJoinPath(target, _countof(target), targetDirectory,
                           identity.name)) return false;
    HANDLE sourceFile = CreateFileW(
        source, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (sourceFile == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sourceSize = {};
    const bool sizeOk = GetFileSizeEx(sourceFile, &sourceSize) &&
        sourceSize.QuadPart >= 0 &&
        static_cast<ULONGLONG>(sourceSize.QuadPart) == identity.size;
    HANDLE targetFile = INVALID_HANDLE_VALUE;
    if (sizeOk) {
        targetFile = CreateFileW(
            target, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    bool copied = targetFile != INVALID_HANDLE_VALUE;
    unsigned char buffer[64 * 1024] = {};
    ULONGLONG remaining = identity.size;
    while (copied && remaining != 0) {
        const DWORD requested = static_cast<DWORD>(
            remaining < sizeof(buffer) ? remaining : sizeof(buffer));
        DWORD read = 0;
        DWORD written = 0;
        copied = ReadFile(sourceFile, buffer, requested, &read, nullptr) &&
                 requested != 0 && read == requested &&
                 WriteFile(targetFile, buffer, read, &written, nullptr) &&
                 written == read;
        if (copied) remaining -= read;
    }
    SecureZeroMemory(buffer, sizeof(buffer));
    if (copied && targetFile != INVALID_HANDLE_VALUE) {
        copied = FlushFileBuffers(targetFile) != FALSE;
    }
    if (targetFile != INVALID_HANDLE_VALUE) CloseHandle(targetFile);
    CloseHandle(sourceFile);
    return copied;
}

// ============================================================
// manifest / report
// ============================================================

static bool SaveGuardWriteManifest(const wchar_t* partialDirectory,
                                   const SaveGuardFileSet& files,
                                   const SaveGuardHashSet& hashes) {
    if (hashes.count != files.count) return false;
    wchar_t path[SAVE_GUARD_PATH_CAP] = {};
    if (!SaveGuardJoinPath(path, _countof(path), partialDirectory,
                           L"manifest.txt")) return false;
    SYSTEMTIME capturedAt = {};
    GetLocalTime(&capturedAt);
    char manifest[32768] = {};
    int used = _snprintf_s(
        manifest, sizeof(manifest), _TRUNCATE,
        "format=VILLAGE_SAVEGUARD_SNAPSHOT_V1\r\n"
        "complete=1\r\n"
        "steam_id=%ls\r\n"
        "captured_local=%04u-%02u-%02uT%02u:%02u:%02u\r\n"
        "file_count=%llu\r\n"
        "total_bytes=%llu\r\n",
        g_guardSteamId, capturedAt.wYear, capturedAt.wMonth,
        capturedAt.wDay, capturedAt.wHour, capturedAt.wMinute,
        capturedAt.wSecond,
        static_cast<unsigned long long>(files.count),
        static_cast<unsigned long long>(files.totalSize));
    if (used <= 0) return false;
    for (size_t index = 0; index < files.count; ++index) {
        char sha256[65] = {};
        SaveGuardSha256Text(hashes.files[index].sha256, sha256);
        const int added = _snprintf_s(
            manifest + used, sizeof(manifest) - static_cast<size_t>(used),
            _TRUNCATE,
            "file=%ls size=%llu sha256=%s last_write_100ns=%llu\r\n",
            files.files[index].name,
            static_cast<unsigned long long>(files.files[index].size),
            sha256,
            static_cast<unsigned long long>(files.files[index].lastWrite));
        if (added <= 0) return false;
        used += added;
    }
    return SaveGuardWriteBytes(path, manifest, static_cast<DWORD>(used));
}

// 损坏现场报告：每个文件的校验结论 + 实测字段 + SHA-256 + 恢复指引
static bool SaveGuardWriteDamagedReport(const wchar_t* partialDirectory,
                                        const SaveGuardFileSet& files,
                                        const SaveGuardHashSet* hashes) {
    wchar_t path[SAVE_GUARD_PATH_CAP] = {};
    if (!SaveGuardJoinPath(path, _countof(path), partialDirectory,
                           L"report.txt")) return false;
    SYSTEMTIME capturedAt = {};
    GetLocalTime(&capturedAt);
    char report[32768] = {};
    int used = _snprintf_s(
        report, sizeof(report), _TRUNCATE,
        "format=VILLAGE_SAVEGUARD_DAMAGED_REPORT_V1\r\n"
        "steam_id=%ls\r\n"
        "captured_local=%04u-%02u-%02uT%02u:%02u:%02u\r\n"
        "file_count=%llu\r\n"
        "expected=\r\n"
        "  save.NNN size=3145728 ver=8\r\n"
        "  .systemsave size=524288 ver=8\r\n"
        "  save.lst size=variable ver=4\r\n"
        "\r\n"
        "[findings]\r\n",
        g_guardSteamId, capturedAt.wYear, capturedAt.wMonth,
        capturedAt.wDay, capturedAt.wHour, capturedAt.wMinute,
        capturedAt.wSecond,
        static_cast<unsigned long long>(files.count));
    if (used <= 0) return false;
    for (size_t index = 0; index < files.count; ++index) {
        const char shaNote[8] = "";
        char shaText[65] = {};
        bool hasSha = false;
        if (hashes && index < hashes->count) {
            SaveGuardSha256Text(hashes->files[index].sha256, shaText);
            hasSha = true;
        }
        const int added = _snprintf_s(
            report + used, sizeof(report) - static_cast<size_t>(used),
            _TRUNCATE,
            "file=%ls result=%s size=%llu ver=%u data_length=%u "
            "expand_limit=%u sha256=%s\r\n",
            g_guardFindings[index].name,
            SaveGuardValidateCodeName(g_guardFindings[index].code),
            static_cast<unsigned long long>(g_guardFindings[index].size),
            g_guardFindings[index].ver, g_guardFindings[index].dataLength,
            g_guardFindings[index].expandLimit,
            hasSha ? shaText : shaNote);
        if (added <= 0) return false;
        used += added;
    }
    const int tail = _snprintf_s(
        report + used, sizeof(report) - static_cast<size_t>(used),
        _TRUNCATE,
        "\r\n[recovery]\r\n"
        "1. Exit the game and Steam completely. Never restore while "
        "the game runs.\r\n"
        "2. Choose the newest snapshot_* folder next to this report; "
        "its manifest.txt must say complete=1.\r\n"
        "3. Copy .systemsave, save.lst, and every save.NNN together "
        "into the live save directory and overwrite matching files.\r\n"
        "4. Start Steam and the game; on a Steam Cloud conflict choose "
        "the restored local files.\r\n");
    if (tail <= 0) return false;
    used += tail;
    return SaveGuardWriteBytes(path, report, static_cast<DWORD>(used));
}

// ============================================================
// 健康快照发布（partial → 复制 → 哈希双检 → manifest → 原子改名）
// ============================================================

// v1.1.3：核心发布逻辑改纯函数化（不再直接写全局，供后台线程执行；
// 主线程退化路径与后台线程执行同一份代码）
static SaveGuardPublishResult SaveGuardPublishSnapshotCore(
        const SaveGuardFileSet& expected,
        SaveGuardFileSet* observedOut, bool* observedValidOut) {
    if (observedOut) *observedOut = {};
    if (observedValidOut) *observedValidOut = false;
    if (!SaveGuardDirectoryIsPlain(g_guardLiveRoot) ||
        !SaveGuardDirectoryIsPlain(g_guardProfileRoot)) {
        return SaveGuardPublishResult::Failed;
    }
    SaveGuardFileSet before = {};
    if (!SaveGuardEnumerateLiveFiles(g_guardLiveRoot, &before)) {
        return SaveGuardPublishResult::Failed;
    }
    if (!SaveGuardFileSetEqual(before, expected)) {
        if (observedOut) *observedOut = before;
        if (observedValidOut) *observedValidOut = true;
        return SaveGuardPublishResult::SourceChanged;
    }

    // 复制前对源做哈希 pass A
    SaveGuardHashSet sourcePassA = {};
    if (!SaveGuardHashFileSet(g_guardLiveRoot, before, &sourcePassA)) {
        return SaveGuardPublishResult::SourceChanged;
    }

    const DWORD processId = GetCurrentProcessId();
    wchar_t partialName[80] = {};
    wchar_t partialDirectory[SAVE_GUARD_PATH_CAP] = {};
    bool partialCreated = false;
    for (unsigned attempt = 0; attempt < 16 && !partialCreated; ++attempt) {
        const LONG sequence = InterlockedIncrement(&g_guardSequence);
        _snwprintf_s(partialName, _countof(partialName), _TRUNCATE,
                     L".partial_%08lX_%08X",
                     static_cast<unsigned long>(processId),
                     static_cast<unsigned>(sequence));
        if (!SaveGuardJoinPath(partialDirectory, _countof(partialDirectory),
                               g_guardProfileRoot, partialName)) {
            return SaveGuardPublishResult::Failed;
        }
        if (CreateDirectoryW(partialDirectory, nullptr)) {
            partialCreated = SaveGuardDirectoryIsPlain(partialDirectory);
        } else if (GetLastError() != ERROR_ALREADY_EXISTS) {
            break;
        }
    }
    if (!partialCreated || !SaveGuardPathIsUnderProfile(partialDirectory)) {
        return SaveGuardPublishResult::Failed;
    }
    wchar_t partialMarker[SAVE_GUARD_PATH_CAP] = {};
    if (!SaveGuardJoinPath(partialMarker, _countof(partialMarker),
                           partialDirectory,
                           SAVE_GUARD_PARTIAL_MARKER_NAME) ||
        !SaveGuardWriteBytes(
            partialMarker, SAVE_GUARD_PARTIAL_MARKER,
            static_cast<DWORD>(sizeof(SAVE_GUARD_PARTIAL_MARKER) - 1))) {
        RemoveDirectoryW(partialDirectory);
        return SaveGuardPublishResult::Failed;
    }

    bool copied = true;
    for (size_t index = 0; index < before.count; ++index) {
        if (!SaveGuardCopyOne(g_guardLiveRoot, partialDirectory,
                              before.files[index])) {
            copied = false;
            break;
        }
    }

    SaveGuardHashSet targetHashes = {};
    const bool targetHashesReadable = copied &&
        SaveGuardHashFileSet(partialDirectory, before, &targetHashes);
    SaveGuardHashSet sourcePassB = {};
    const bool sourcePassBReadable =
        SaveGuardHashFileSet(g_guardLiveRoot, before, &sourcePassB);
    SaveGuardFileSet after = {};
    const bool afterReadable =
        SaveGuardEnumerateLiveFiles(g_guardLiveRoot, &after);
    const bool directoryUnchanged =
        afterReadable && SaveGuardFileSetEqual(before, after);
    const bool sourceContentUnchanged = sourcePassBReadable &&
        SaveGuardHashSetEqual(sourcePassA, sourcePassB);
    const bool targetMatchesSource = targetHashesReadable &&
        SaveGuardHashSetEqual(sourcePassA, targetHashes);
    if (!copied || !directoryUnchanged || !sourceContentUnchanged ||
        !targetMatchesSource) {
        SaveGuardRemoveOwnedSnapshot(partialDirectory, true);
        if (!directoryUnchanged || !sourceContentUnchanged) {
            if (observedOut && afterReadable) {
                *observedOut = after;
                *observedValidOut = true;
            }
            return SaveGuardPublishResult::SourceChanged;
        }
        return SaveGuardPublishResult::Failed;
    }

    if (!SaveGuardWriteManifest(partialDirectory, before, sourcePassA)) {
        SaveGuardRemoveOwnedSnapshot(partialDirectory, true);
        return SaveGuardPublishResult::Failed;
    }
    wchar_t finalMarker[SAVE_GUARD_PATH_CAP] = {};
    if (!SaveGuardJoinPath(finalMarker, _countof(finalMarker),
                           partialDirectory, SAVE_GUARD_FINAL_MARKER_NAME) ||
        !SaveGuardWriteBytes(
            finalMarker, SAVE_GUARD_FINAL_MARKER,
            static_cast<DWORD>(sizeof(SAVE_GUARD_FINAL_MARKER) - 1)) ||
        !DeleteFileW(partialMarker)) {
        SaveGuardRemoveOwnedSnapshot(partialDirectory, true);
        return SaveGuardPublishResult::Failed;
    }

    SYSTEMTIME capturedAt = {};
    GetLocalTime(&capturedAt);
    wchar_t finalName[96] = {};
    _snwprintf_s(finalName, _countof(finalName), _TRUNCATE,
                 L"snapshot_%04u%02u%02u_%02u%02u%02u_%04X",
                 capturedAt.wYear, capturedAt.wMonth, capturedAt.wDay,
                 capturedAt.wHour, capturedAt.wMinute, capturedAt.wSecond,
                 g_guardSequence & 0xffffu);
    wchar_t finalDirectory[SAVE_GUARD_PATH_CAP] = {};
    if (!SaveGuardJoinPath(finalDirectory, _countof(finalDirectory),
                           g_guardProfileRoot, finalName) ||
        !MoveFileExW(partialDirectory, finalDirectory,
                     MOVEFILE_WRITE_THROUGH)) {
        SaveGuardRemoveOwnedSnapshot(partialDirectory, true);
        return SaveGuardPublishResult::Failed;
    }

    SaveGuardPruneSnapshots(SAVE_GUARD_SNAPSHOT_PREFIX,
                            SAVE_GUARD_MAX_SNAPSHOTS);
    Log("[SaveGuard] snapshot_published files=%llu bytes=%llu "
        "path=%ls sha256_a_target_b=1 complete=1 bg=1\n",
        static_cast<unsigned long long>(before.count),
        static_cast<unsigned long long>(before.totalSize), finalDirectory);
    return SaveGuardPublishResult::Published;
}

// ============================================================
// v1.1.3：后台快照线程（纯文件 IO，不碰游戏内存/原生函数——线程安全；
// 复制+双轮哈希 25.7MB 不再冻结主线程）
// ============================================================
static DWORD WINAPI SaveGuardPublishThreadProc(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    while (true) {
        EnterCriticalSection(&g_guardPublishLock);
        while (g_guardPublishState != 1) {
            SleepConditionVariableCS(&g_guardPublishWake,
                                     &g_guardPublishLock, 1000);
        }
        SaveGuardFileSet request = g_guardPublishRequest;
        LeaveCriticalSection(&g_guardPublishLock);

        SaveGuardFileSet observed = {};
        bool observedValid = false;
        const SaveGuardPublishResult result =
            SaveGuardPublishSnapshotCore(request, &observed, &observedValid);

        EnterCriticalSection(&g_guardPublishLock);
        g_guardPublishResult = result;
        g_guardPublishObserved = observed;
        g_guardPublishObservedValid = observedValid;
        g_guardPublishState = 2;
        LeaveCriticalSection(&g_guardPublishLock);
        // 主线程 Pump 轮询到 2 后应用并复位为 0
    }
}

// 提交快照请求（返回 false=线程不可用已退化为主线程同步执行）
static void SaveGuardSubmitSnapshot(const SaveGuardFileSet& expected) {
    if (!g_guardPublishThread) {
        InitializeCriticalSection(&g_guardPublishLock);
        InitializeConditionVariable(&g_guardPublishWake);
        g_guardPublishLockReady = true;
        g_guardPublishThread = CreateThread(
            nullptr, 0, SaveGuardPublishThreadProc, nullptr, 0, nullptr);
        if (!g_guardPublishThread) {
            // 退化路径：同步执行（v1.1.2 原行为，保底）
            SaveGuardFileSet observed = {};
            bool observedValid = false;
            const SaveGuardPublishResult result =
                SaveGuardPublishSnapshotCore(expected, &observed,
                                             &observedValid);
            if (result == SaveGuardPublishResult::SourceChanged) {
                SaveGuardFileSet current2 = {};
                if (SaveGuardEnumerateLiveFiles(g_guardLiveRoot, &current2))
                    g_guardObserved = current2;
                g_guardStableSince = GetTickCount64();
            } else if (result == SaveGuardPublishResult::Failed) {
                g_guardNextPollAt = GetTickCount64() + SAVE_GUARD_RETRY_MS;
                Log("[SaveGuard] snapshot_retry "
                    "reason=COPY_OR_VALIDATION_FAILED sync_fallback=1\n");
            }
            return;
        }
    }
    if (InterlockedCompareExchange(&g_guardPublishState, 1, 0) != 0) {
        return;  // 上一轮仍在执行，本轮放弃（幂等短路下轮自然重试）
    }
    EnterCriticalSection(&g_guardPublishLock);
    g_guardPublishRequest = expected;
    LeaveCriticalSection(&g_guardPublishLock);
    WakeConditionVariable(&g_guardPublishWake);
}

// ============================================================
// 损坏现场发布（取证优先：好坏文件都原样复制 + report.txt）
// ============================================================

static void SaveGuardPublishDamaged(const SaveGuardFileSet& corrupted) {
    if (!SaveGuardDirectoryIsPlain(g_guardLiveRoot) ||
        !SaveGuardDirectoryIsPlain(g_guardProfileRoot)) return;

    // 重新枚举：若期间游戏又在写盘，放弃本轮抢救，等待重新稳定
    SaveGuardFileSet before = {};
    if (!SaveGuardEnumerateLiveFiles(g_guardLiveRoot, &before) ||
        !SaveGuardFileSetEqual(before, corrupted)) {
        g_guardObserved = before;
        g_guardStableSince = GetTickCount64();
        Log("[SaveGuard] damaged_capture_deferred reason=SOURCE_CHANGING\n");
        return;
    }

    const DWORD processId = GetCurrentProcessId();
    wchar_t partialName[80] = {};
    wchar_t partialDirectory[SAVE_GUARD_PATH_CAP] = {};
    bool partialCreated = false;
    for (unsigned attempt = 0; attempt < 16 && !partialCreated; ++attempt) {
        const LONG sequence = InterlockedIncrement(&g_guardSequence);
        _snwprintf_s(partialName, _countof(partialName), _TRUNCATE,
                     L".partial_%08lX_%08X",
                     static_cast<unsigned long>(processId), sequence);
        if (!SaveGuardJoinPath(partialDirectory, _countof(partialDirectory),
                               g_guardProfileRoot, partialName)) return;
        if (CreateDirectoryW(partialDirectory, nullptr)) {
            partialCreated = SaveGuardDirectoryIsPlain(partialDirectory);
        } else if (GetLastError() != ERROR_ALREADY_EXISTS) {
            break;
        }
    }
    if (!partialCreated || !SaveGuardPathIsUnderProfile(partialDirectory)) {
        return;
    }
    wchar_t partialMarker[SAVE_GUARD_PATH_CAP] = {};
    if (!SaveGuardJoinPath(partialMarker, _countof(partialMarker),
                           partialDirectory,
                           SAVE_GUARD_PARTIAL_MARKER_NAME) ||
        !SaveGuardWriteBytes(
            partialMarker, SAVE_GUARD_PARTIAL_MARKER,
            static_cast<DWORD>(sizeof(SAVE_GUARD_PARTIAL_MARKER) - 1))) {
        RemoveDirectoryW(partialDirectory);
        return;
    }

    // 全量复制（好坏都留档，给后续存档修复 MOD 留真实样本）
    unsigned copyFailed = 0;
    for (size_t index = 0; index < before.count; ++index) {
        if (!SaveGuardCopyOneRaw(g_guardLiveRoot, partialDirectory,
                                 before.files[index])) {
            ++copyFailed;
            Log("[SaveGuard] damaged_copy_failed file=%ls win32=%lu\n",
                before.files[index].name, GetLastError());
        }
    }

    // 每个文件哈希进 report（损坏文件的哈希同样有取证价值）
    SaveGuardHashSet hashes = {};
    SaveGuardHashFileSet(partialDirectory, before, &hashes);

    if (!SaveGuardWriteDamagedReport(partialDirectory, before, &hashes)) {
        Log("[SaveGuard] damaged_report_write_failed win32=%lu\n",
            GetLastError());
    }

    wchar_t finalMarker[SAVE_GUARD_PATH_CAP] = {};
    if (!SaveGuardJoinPath(finalMarker, _countof(finalMarker),
                           partialDirectory, SAVE_GUARD_FINAL_MARKER_NAME) ||
        !SaveGuardWriteBytes(
            finalMarker, SAVE_GUARD_FINAL_MARKER,
            static_cast<DWORD>(sizeof(SAVE_GUARD_FINAL_MARKER) - 1)) ||
        !DeleteFileW(partialMarker)) {
        SaveGuardRemoveOwnedSnapshot(partialDirectory, true);
        return;
    }

    SYSTEMTIME capturedAt = {};
    GetLocalTime(&capturedAt);
    wchar_t finalName[96] = {};
    _snwprintf_s(finalName, _countof(finalName), _TRUNCATE,
                 L"damaged_%04u%02u%02u_%02u%02u%02u_%04X",
                 capturedAt.wYear, capturedAt.wMonth, capturedAt.wDay,
                 capturedAt.wHour, capturedAt.wMinute, capturedAt.wSecond,
                 g_guardSequence & 0xffffu);
    wchar_t finalDirectory[SAVE_GUARD_PATH_CAP] = {};
    if (!SaveGuardJoinPath(finalDirectory, _countof(finalDirectory),
                           g_guardProfileRoot, finalName) ||
        !MoveFileExW(partialDirectory, finalDirectory,
                     MOVEFILE_WRITE_THROUGH)) {
        SaveGuardRemoveOwnedSnapshot(partialDirectory, true);
        return;
    }

    SaveGuardPruneSnapshots(SAVE_GUARD_DAMAGED_PREFIX,
                            SAVE_GUARD_MAX_DAMAGED);
    Log("[SaveGuard] damaged_evidence_published files=%llu "
        "copy_failed=%u path=%ls\n",
        static_cast<unsigned long long>(before.count), copyFailed,
        finalDirectory);
}

// ============================================================
// README（备份根目录）
// ============================================================

static bool SaveGuardWriteReadme() {
    wchar_t path[SAVE_GUARD_PATH_CAP] = {};
    if (!SaveGuardJoinPath(path, _countof(path), g_guardBackupRoot,
                           L"README_如何恢复存档.txt")) return false;
    static constexpr wchar_t text[] =
        L"Village in the Shade - SaveGuard 存档巡检 / Save Patrol\r\n"
        L"=======================================================\r\n\r\n"
        L"本目录由 Village QoL Mod 的 SaveGuard 组件自动维护：\r\n"
        L"Mod 绝不改写正在使用的原存档，只在做完整性校验时复制快照。\r\n\r\n"
        L"snapshot_*  是存档组全部通过 YKCMP 容器校验（魔数/固定长度/\r\n"
        L"ver/数据长度）后发布的健康快照，manifest.txt 记录每个文件的\r\n"
        L"SHA-256，complete=1 表示复制时源与目标哈希一致。\r\n"
        L"damaged_*   是检测到存档文件异常（连续两次校验失败）时的\r\n"
        L"现场取证：好坏文件都原样保留，report.txt 列出每个文件的\r\n"
        L"实测字段与失败原因。\r\n\r\n"
        L"恢复步骤（务必完整执行）：\r\n"
        L"1. 正常退出游戏，再退出 Steam；不要在游戏运行时恢复。\r\n"
        L"2. 暂停该游戏的 Steam Cloud，或准备在云冲突时选择本地文件。\r\n"
        L"3. 先手动复制一份当前存档目录，作为恢复前保险。\r\n"
        L"4. 在 <SteamID64> 目录选择最新的 snapshot_*，确认 manifest.txt\r\n"
        L"   里 complete=1。\r\n"
        L"5. 将快照中的 .systemsave、save.lst、全部 save.NNN 一起复制到：\r\n"
        L"   %APPDATA%\\Nippon Ichi Software, Inc\\Honogurashinoniwa\\<SteamID64>\r\n"
        L"   并覆盖同名文件。不要只恢复个别文件；save.lst 含槽位元数据。\r\n"
        L"6. 启动 Steam 和游戏。若 Steam Cloud 冲突，选择刚恢复的本地文件。\r\n\r\n"
        L"提示：damaged_* 是异常现场，通常不要用于恢复；若 report.txt 显示\r\n"
        L"只有个别槽位损坏，其余文件是从同一现场复制的，可作参考。\r\n";
    return SaveGuardWriteUtf8(path, text);
}

// ============================================================
// 初始化 / 巡检主循环
// ============================================================

static bool SaveGuardPrepareProfileRoot() {
    if (!g_guardSteamId[0] || !g_guardLiveRoot[0] ||
        !SaveGuardJoinPath(g_guardProfileRoot,
                           _countof(g_guardProfileRoot),
                           g_guardBackupRoot, g_guardSteamId) ||
        !SaveGuardEnsureDirectory(g_guardProfileRoot)) return false;
    SaveGuardFileSet initial = {};
    if (!SaveGuardEnumerateLiveFiles(g_guardLiveRoot, &initial)) {
        return false;
    }
    g_guardObserved = initial;
    g_guardStableSince = GetTickCount64();
    g_guardValidatedFresh = false;
    g_guardFirstFailureAt = 0;
    g_guardSourceReady = true;
    g_guardSourceMissingLogged = false;
    Log("[SaveGuard] source_ready steam_id=%ls files=%llu bytes=%llu "
        "stable_ms=%llu\n",
        g_guardSteamId,
        static_cast<unsigned long long>(initial.count),
        static_cast<unsigned long long>(initial.totalSize),
        static_cast<unsigned long long>(SAVE_GUARD_STABLE_MS));
    return true;
}

static bool InitializeSaveGuard() {
    if (g_guardInitialized) return true;
    if (!PackageWritesAuthorized() || !g_module) return false;
    wchar_t productRoot[SAVE_GUARD_PATH_CAP] = {};
    if (!SaveGuardBuildProductRoot(productRoot, _countof(productRoot))) {
        Log("[SaveGuard] initialize_failed stage=product_root win32=%lu\n",
            static_cast<unsigned long>(GetLastError()));
        return false;
    }
    if (!SaveGuardJoinPath(g_guardBackupRoot, _countof(g_guardBackupRoot),
                           productRoot, SAVE_GUARD_BACKUP_DIR_NAME) ||
        !SaveGuardEnsureDirectory(g_guardBackupRoot) ||
        !SaveGuardWriteReadme()) {
        Log("[SaveGuard] initialize_failed stage=backup_root_or_readme "
            "win32=%lu\n", static_cast<unsigned long>(GetLastError()));
        return false;
    }
    g_guardInitialized = true;
    g_guardNextPollAt = GetTickCount64();
    g_guardNextAccountRetryAt = g_guardNextPollAt;
    if (SaveGuardResolveLiveProfile() && SaveGuardPrepareProfileRoot()) {
        Log("[SaveGuard] initialized backup_root=%ls snapshot_keep=%llu "
            "damaged_keep=%llu live_save_writes=0\n",
            g_guardBackupRoot,
            static_cast<unsigned long long>(SAVE_GUARD_MAX_SNAPSHOTS),
            static_cast<unsigned long long>(SAVE_GUARD_MAX_DAMAGED));
    } else {
        g_guardLiveRoot[0] = L'\0';
        g_guardSteamId[0] = L'\0';
        g_guardProfileRoot[0] = L'\0';
        g_guardSourceReady = false;
        Log("[SaveGuard] initialized backup_root=%ls source_pending=1 "
            "live_save_writes=0\n", g_guardBackupRoot);
    }
    return true;
}

static void PumpSaveGuard() {
    if (!g_guardInitialized || !PackageWritesAuthorized()) return;
    const ULONGLONG now = GetTickCount64();
    if (now < g_guardNextPollAt) return;
    g_guardNextPollAt = now + SAVE_GUARD_POLL_MS;

    // v1.1.3：应用后台发布结果（主线程侧唯一写全局点）
    if (g_guardPublishThread &&
        InterlockedCompareExchange(&g_guardPublishState, 3, 2) == 2) {
        EnterCriticalSection(&g_guardPublishLock);
        const SaveGuardPublishResult result = g_guardPublishResult;
        if (result == SaveGuardPublishResult::SourceChanged &&
            g_guardPublishObservedValid) {
            g_guardObserved = g_guardPublishObserved;
        }
        const bool failed = result == SaveGuardPublishResult::Failed;
        g_guardPublishState = 0;
        LeaveCriticalSection(&g_guardPublishLock);
        if (result == SaveGuardPublishResult::SourceChanged) {
            g_guardStableSince = GetTickCount64();
        } else if (failed) {
            g_guardNextPollAt = GetTickCount64() + SAVE_GUARD_RETRY_MS;
            Log("[SaveGuard] snapshot_retry "
                "reason=COPY_OR_VALIDATION_FAILED bg=1\n");
        }
    }

    // 存档目录解析（新装玩家首次启动 / Steam 未就绪时周期重试）
    if (!g_guardSourceReady) {
        if (now < g_guardNextAccountRetryAt) return;
        g_guardNextAccountRetryAt = now + SAVE_GUARD_ACCOUNT_RETRY_MS;
        if (!SaveGuardResolveLiveProfile() ||
            !SaveGuardPrepareProfileRoot()) {
            if (!g_guardSourceMissingLogged) {
                g_guardSourceMissingLogged = true;
                Log("[SaveGuard] source_waiting appdata_profile_unavailable=1\n");
            }
            g_guardLiveRoot[0] = L'\0';
            g_guardSteamId[0] = L'\0';
            g_guardProfileRoot[0] = L'\0';
            return;
        }
    }

    SaveGuardFileSet current = {};
    if (!SaveGuardEnumerateLiveFiles(g_guardLiveRoot, &current)) {
        if (!g_guardSourceMissingLogged) {
            g_guardSourceMissingLogged = true;
            Log("[SaveGuard] source_unstable_or_unavailable retry=1\n");
        }
        return;
    }
    g_guardSourceMissingLogged = false;

    // v1.1.3：只有真存档组（save.NNN/save.lst）变化才重置稳定计时并记日志；
    // .systemsave 独自变化（高频）静默跟踪，不触发校验/快照链。
    if (!SaveGuardSlotFilesEqual(current, g_guardObserved)) {
        g_guardObserved = current;
        g_guardStableSince = now;
        const bool damageTracked = g_guardDamagedFresh;
        g_guardValidatedFresh = false;
        Log("[SaveGuard] live_change_observed files=%llu bytes=%llu "
            "stable_ms=%llu damage_tracked=%d\n",
            static_cast<unsigned long long>(current.count),
            static_cast<unsigned long long>(current.totalSize),
            static_cast<unsigned long long>(SAVE_GUARD_STABLE_MS),
            damageTracked ? 1 : 0);
        return;
    }
    g_guardObserved = current;  // 静默跟踪（含 .systemsave 漂移）

    // 未稳定 → 继续等待
    if (now - g_guardStableSince < SAVE_GUARD_STABLE_MS) return;

    // 幂等短路：真存档组未变则不重复处理（v1.1.3 改 slot 视图——否则
    // .systemsave 漂移会击穿短路，每 2.5s 重复一轮校验+全量快照）
    if (g_guardValidatedFresh &&
        SaveGuardSlotFilesEqual(current, g_guardValidatedSet)) return;
    if (g_guardDamagedFresh &&
        SaveGuardSlotFilesEqual(current, g_guardDamagedSet)) return;

    // 首次失败后的重测窗口（连续两次失败才定性，防瞬时误报）
    if (g_guardFirstFailureAt != 0 &&
        now - g_guardFirstFailureAt < SAVE_GUARD_RETEST_MS) return;

    // 稳定后执行整组校验
    const bool allHealthy = SaveGuardValidateSet(g_guardLiveRoot, current);

    if (allHealthy) {
        g_guardFirstFailureAt = 0;
        const bool wasDamagedState = g_guardDamagedFresh;
        g_guardValidatedSet = current;
        g_guardValidatedFresh = true;
        if (wasDamagedState &&
            !SaveGuardSlotFilesEqual(current, g_guardDamagedSet)) {
            Log("[SaveGuard] damage_resolved files_now_healthy=1\n");
        }
        // v1.1.3：提交后台快照（25.7MB 复制+哈希在后台线程，主线程零冻结；
        // 完成/失败由 Pump 开头统一应用，Published 日志由后台线程直打）
        SaveGuardSubmitSnapshot(current);
        return;
    }

    // 存在损坏
    if (g_guardFirstFailureAt == 0) {
        g_guardFirstFailureAt = now;
        for (size_t index = 0; index < current.count; ++index) {
            if (g_guardFindings[index].code ==
                SaveGuardValidateCode::Passed) continue;
            Log("[SaveGuard] suspicious_save_file file=%ls code=%s "
                "size=%llu ver=%u data_length=%u expand_limit=%u "
                "retest_in_ms=%llu\n",
                g_guardFindings[index].name,
                SaveGuardValidateCodeName(g_guardFindings[index].code),
                static_cast<unsigned long long>(g_guardFindings[index].size),
                g_guardFindings[index].ver,
                g_guardFindings[index].dataLength,
                g_guardFindings[index].expandLimit,
                static_cast<unsigned long long>(SAVE_GUARD_RETEST_MS));
        }
        return;
    }

    // 第二次校验仍失败 → 定性损坏：告警 + 抢救现场
    g_guardDamagedSet = current;
    g_guardDamagedFresh = true;
    g_guardFirstFailureAt = 0;
    for (size_t index = 0; index < current.count; ++index) {
        if (g_guardFindings[index].code ==
            SaveGuardValidateCode::Passed) continue;
        LogE("[SaveGuard] ALERT damaged_save_file file=%ls code=%s "
             "size=%llu ver=%u data_length=%u expand_limit=%u "
             "action=evidence_then_keep_last_healthy_snapshot\n",
             g_guardFindings[index].name,
             SaveGuardValidateCodeName(g_guardFindings[index].code),
             static_cast<unsigned long long>(g_guardFindings[index].size),
             g_guardFindings[index].ver,
             g_guardFindings[index].dataLength,
             g_guardFindings[index].expandLimit);
    }
    SaveGuardPublishDamaged(current);
}
