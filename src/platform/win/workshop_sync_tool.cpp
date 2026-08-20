// workshop_sync_tool.exe - push/pull CloudRedirect save data to/from a Steam
// Workshop item.
//
// One Workshop item per (user, game): the item title is the HMAC-SHA256 of
// (steamid64:gameAppid) under a fixed project salt, prefixed with
// "CloudRedirect:". On a new machine the tool recomputes the same title and
// finds the item by enumerating the user's published items, so no item id has
// to be stored anywhere. The item is Unlisted (link-only) by default.
//
// Resolves flat C exports from a bundled 64-bit steam_api64.dll at runtime,
// same pattern as cloud760_tool.exe. steam_api64.dll must sit next to this exe.
//
// Commands (machine-readable output on stdout, errors on stderr):
//   push --content <dir> --game-appid <n> [--workshop-appid <n>] [--visibility <n>] [--account <acct>]
//       Create-or-update the item for (steamid64, gameAppid). On success
//       prints "ITEM <id>". Item content is replaced by <dir>.
//   pull --content <dir> --game-appid <n> [--workshop-appid <n>] [--account <acct>] [--timeout-sec <n>]
//       Subscribe/download the item and merge newer files into <dir>.
//       Prints "ITEM <id>" or "NONE" (no item published yet).
//   find --game-appid <n> [--workshop-appid <n>]
//       Prints "ITEM <id>" + "URL ..." if the item exists, else "NONE".
//   list [--workshop-appid <n>]
//       Lists all CloudRedirect items published under the workshop appid.
//
// Exit codes: 0 ok (NONE included), 2 workshop legal agreement needed,
// 3 Steam not running / not logged in, 4 rate limited (retry later),
// 5 account mismatch (--account), 1 any other error.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <functional>

#include "sha256_hmac.h"

// ── Encoding helpers ───────────────────────────────────────────────────
// The provider spawns this tool with a UTF-8 command line (CreateProcessW).
// wmain keeps the arguments lossless. Steam API strings are UTF-8, while
// Win32 file APIs need wide strings converted from UTF-8. Round-tripping
// through the ANSI code page is what broke uploads on non-ASCII (Chinese)
// content paths: GetFileAttributesA found the folder but SetItemContent
// received mojibake and the submit failed with EResult=2.

static std::string ToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string s((size_t)(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

static std::string ToUtf8(const std::wstring& w) {
    return ToUtf8(w.c_str());
}

static std::wstring ToWide(const char* utf8) {
    if (!utf8 || !*utf8) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring s((size_t)(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, s.data(), len);
    return s;
}

static std::wstring ToWide(const std::string& utf8) {
    return ToWide(utf8.c_str());
}

// Steam returns install-info folder paths as char*; the convention is UTF-8,
// but be tolerant of ACP bytes from older builds.
static std::wstring ToWideTolerant(const char* p) {
    std::wstring w = ToWide(p);
    if (!w.empty()) return w;
    if (!p || !*p) return {};
    int len = MultiByteToWideChar(CP_ACP, 0, p, -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring s((size_t)(len - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, p, -1, s.data(), len);
    return s;
}

// ── Flat Steamworks API typedefs (subset we use) ───────────────────────
// Signatures match steam_api_flat.h from the Steamworks SDK.
typedef int32_t HSteamUser;
typedef int32_t HSteamPipe;
typedef uint32_t AppId_t;
typedef uint32_t AccountID_t;
typedef uint64_t PublishedFileId_t;
typedef uint64_t SteamAPICall_t;
typedef uint64_t UGCUpdateHandle_t;
typedef uint64_t UGCQueryHandle_t;
typedef uint64_t UGCHandle_t;

using ISteamUGC = void;
using ISteamUser = void;
using ISteamUtils = void;

typedef bool   (__cdecl* SteamAPI_Init_t)();
typedef void   (__cdecl* SteamAPI_Shutdown_t)();
typedef void   (__cdecl* SteamAPI_RunCallbacks_t)();
typedef HSteamUser (__cdecl* SteamAPI_GetHSteamUser_t)();
typedef HSteamPipe (__cdecl* SteamAPI_GetHSteamPipe_t)();
typedef void*  (__cdecl* SteamClient_t)();
typedef void*  (__cdecl* SteamAPI_ISteamClient_GetISteamUGC_t)(void*, HSteamUser, HSteamPipe, const char*);
typedef void*  (__cdecl* SteamAPI_ISteamClient_GetISteamUser_t)(void*, HSteamUser, HSteamPipe, const char*);
typedef void*  (__cdecl* SteamAPI_ISteamClient_GetISteamUtils_t)(void*, HSteamUser, HSteamPipe, const char*);
// Canonical interface factory; works for every interface, every build.
typedef void*  (__cdecl* SteamAPI_ISteamClient_GetISteamGenericInterface_t)(void*, HSteamUser, HSteamPipe, const char*);

typedef SteamAPICall_t (__cdecl* UGC_CreateItem_t)(ISteamUGC*, AppId_t, int32_t);
typedef UGCUpdateHandle_t (__cdecl* UGC_StartItemUpdate_t)(ISteamUGC*, AppId_t, PublishedFileId_t);
typedef bool   (__cdecl* UGC_SetItemTitle_t)(ISteamUGC*, UGCUpdateHandle_t, const char*);
typedef bool   (__cdecl* UGC_SetItemDescription_t)(ISteamUGC*, UGCUpdateHandle_t, const char*);
typedef bool   (__cdecl* UGC_SetItemVisibility_t)(ISteamUGC*, UGCUpdateHandle_t, int32_t);
typedef bool   (__cdecl* UGC_SetItemContent_t)(ISteamUGC*, UGCUpdateHandle_t, const char*);
typedef SteamAPICall_t (__cdecl* UGC_SubmitItemUpdate_t)(ISteamUGC*, UGCUpdateHandle_t, const char*);
typedef int32_t (__cdecl* UGC_GetItemUpdateProgress_t)(ISteamUGC*, UGCUpdateHandle_t, uint64_t*, uint64_t*);
typedef UGCQueryHandle_t (__cdecl* UGC_CreateQueryUserUGCRequest_t)(ISteamUGC*, AccountID_t, int32_t, uint32_t, int32_t, AppId_t, AppId_t, uint32_t);
typedef SteamAPICall_t (__cdecl* UGC_SendQueryUGCRequest_t)(ISteamUGC*, UGCQueryHandle_t);
typedef bool   (__cdecl* UGC_ReleaseQueryUGCRequest_t)(ISteamUGC*, UGCQueryHandle_t);
// NOTE: GetNumQueryUGCResults has no flat export in the bundled 2021
// steam_api64.dll, so result counts come from GetQueryUGCResult iteration.
typedef bool   (__cdecl* UGC_GetQueryUGCResult_t)(ISteamUGC*, UGCQueryHandle_t, uint32_t, void*);
typedef bool   (__cdecl* UGC_SubscribeItem_t)(ISteamUGC*, PublishedFileId_t);
typedef uint32_t (__cdecl* UGC_GetItemState_t)(ISteamUGC*, PublishedFileId_t);
typedef bool   (__cdecl* UGC_GetItemInstallInfo_t)(ISteamUGC*, PublishedFileId_t, uint64_t*, char*, uint32_t, uint32_t*);
typedef bool   (__cdecl* UGC_DownloadItem_t)(ISteamUGC*, PublishedFileId_t, bool);

typedef uint64_t (__cdecl* User_GetSteamID_t)(ISteamUser*);
typedef bool   (__cdecl* Utils_GetAPICallResult_t)(ISteamUtils*, SteamAPICall_t, void*, int, int, bool*);

// ── Steamworks public structs (layouts stable across SDK versions) ─────

// EResult subset (values from steamclientpublic.h)
enum EResult {
    k_EResultOK = 1, k_EResultFail = 2, k_EResultInvalidParam = 8,
    k_EResultFileNotFound = 9, k_EResultBusy = 10,
    k_EResultAccessDenied = 15, k_EResultBanned = 17,
    k_EResultNotLoggedOn = 21, k_EResultInsufficientPrivilege = 24,
    k_EResultLimitExceeded = 25, k_EResultRateLimitExceeded = 84,
};

// EWorkshopFileType
enum EWorkshopFileType {
    k_EWorkshopFileTypeCommunity = 0,
};

// ERemoteStoragePublishedFileVisibility
enum ERemoteStoragePublishedFileVisibility {
    k_ERemoteStoragePublishedFileVisibilityPublic = 0,
    k_ERemoteStoragePublishedFileVisibilityFriendsOnly = 1,
    k_ERemoteStoragePublishedFileVisibilityPrivate = 2,
    k_ERemoteStoragePublishedFileVisibilityUnlisted = 3,
};

// EUserUGCList
enum EUserUGCList {
    k_EUserUGCList_Published = 0,
};

// EUGCMatchingUGCType
enum EUGCMatchingUGCType {
    k_EUGCMatchingUGCType_Items = 0,
    k_EUGCMatchingUGCType_All = 0xFFFFFFFFu,
};

// EUserUGCListSortOrder
enum EUserUGCListSortOrder {
    k_EUserUGCListSortOrder_CreationOrderDesc = 0,
    k_EUserUGCListSortOrder_TitleAsc = 11,
};

// EItemState
enum EItemState {
    k_EItemStateNone = 0,
    k_EItemStateSubscribed = 1,
    k_EItemStateInstalled = 4,
    k_EItemStateNeedsUpdate = 8,
    k_EItemStateDownloading = 16,
    k_EItemStateDownloadPending = 32,
};

struct SteamUGCDetails_t {
    PublishedFileId_t m_nPublishedFileId;
    EResult m_eResult;
    EWorkshopFileType m_eFileType;
    AppId_t m_nCreatorAppID;
    AppId_t m_nConsumerAppID;
    char m_rgchTitle[129];
    char m_rgchDescription[8000];
    uint64_t m_ullSteamIDOwner;
    uint32_t m_rtimeCreated;
    uint32_t m_rtimeUpdated;
    uint32_t m_rtimeAddedToUserList;
    ERemoteStoragePublishedFileVisibility m_eVisibility;
    bool m_bBanned;
    bool m_bAcceptedForUse;
    bool m_bTagsTruncated;
    char m_rgchTags[1025];
    UGCHandle_t m_hFile;
    UGCHandle_t m_hPreviewFile;
    char m_pchFileName[260];
    int32_t m_nFileSize;
    int32_t m_nPreviewFileSize;
    char m_rgchURL[256];
    uint32_t m_unVotesUp;
    uint32_t m_unVotesDown;
    float m_flScore;
    uint32_t m_unNumChildren;
};

// k_iCallback values (public, stable)
struct CreateItemResult_t {
    EResult m_eResult;
    PublishedFileId_t m_nPublishedFileId;
    bool m_bUserNeedsToAcceptWorkshopLegalAgreement;
    static const int k_iCallback = 3403;
};

struct SubmitItemUpdateResult_t {
    EResult m_eResult;
    bool m_bUserNeedsToAcceptWorkshopLegalAgreement;
    PublishedFileId_t m_nPublishedFileId;
    static const int k_iCallback = 3404;
};

struct UGCQueryCompleted_t {
    UGCQueryHandle_t m_handle;
    EResult m_eResult;
    uint32_t m_unNumResultsReturned;
    uint32_t m_unTotalMatchingResults;
    bool m_bCachedData;
    char m_rgchNextCursor[256];
    static const int k_iCallback = 3401;
};

struct SteamApi {
    HMODULE mod = nullptr;
    SteamAPI_Init_t Init = nullptr;
    SteamAPI_Shutdown_t Shutdown = nullptr;
    SteamAPI_RunCallbacks_t RunCallbacks = nullptr;
    SteamAPI_GetHSteamUser_t GetHSteamUser = nullptr;
    SteamAPI_GetHSteamPipe_t GetHSteamPipe = nullptr;
    SteamClient_t SteamClient = nullptr;
    SteamAPI_ISteamClient_GetISteamUGC_t GetISteamUGC = nullptr;
    SteamAPI_ISteamClient_GetISteamUser_t GetISteamUser = nullptr;
    SteamAPI_ISteamClient_GetISteamUtils_t GetISteamUtils = nullptr;
    SteamAPI_ISteamClient_GetISteamGenericInterface_t GetGenericInterface = nullptr;

    UGC_CreateItem_t CreateItem = nullptr;
    UGC_StartItemUpdate_t StartItemUpdate = nullptr;
    UGC_SetItemTitle_t SetItemTitle = nullptr;
    UGC_SetItemDescription_t SetItemDescription = nullptr;
    UGC_SetItemVisibility_t SetItemVisibility = nullptr;
    UGC_SetItemContent_t SetItemContent = nullptr;
    UGC_SubmitItemUpdate_t SubmitItemUpdate = nullptr;
    UGC_GetItemUpdateProgress_t GetItemUpdateProgress = nullptr;
    UGC_CreateQueryUserUGCRequest_t CreateQueryUserUGCRequest = nullptr;
    UGC_SendQueryUGCRequest_t SendQueryUGCRequest = nullptr;
    UGC_ReleaseQueryUGCRequest_t ReleaseQueryUGCRequest = nullptr;
    UGC_GetQueryUGCResult_t GetQueryUGCResult = nullptr;
    UGC_SubscribeItem_t SubscribeItem = nullptr;
    UGC_GetItemState_t GetItemState = nullptr;
    UGC_GetItemInstallInfo_t GetItemInstallInfo = nullptr;
    UGC_DownloadItem_t DownloadItem = nullptr;

    User_GetSteamID_t GetSteamID = nullptr;
    Utils_GetAPICallResult_t GetAPICallResult = nullptr;

    ISteamUGC* ugc = nullptr;
    ISteamUser* user = nullptr;
    ISteamUtils* utils = nullptr;
};

template <typename T>
static bool resolve(HMODULE m, const char* name, T& out) {
    out = reinterpret_cast<T>(GetProcAddress(m, name));
    if (!out) {
        fprintf(stderr, "Error: missing export %s in steam_api64.dll\n", name);
        return false;
    }
    return true;
}

// Load bundled steam_api64.dll from exe directory (no game-folder search).
static HMODULE LoadSteamApiDll() {
    wchar_t exeDir[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wchar_t* slash = wcsrchr(exeDir, L'\\');
        if (slash) {
            *(slash + 1) = L'\0';
            std::wstring full = std::wstring(exeDir) + L"steam_api64.dll";
            HMODULE m = LoadLibraryW(full.c_str());
            if (m) return m;
        }
    }
    return LoadLibraryW(L"steam_api64.dll");
}

static bool LoadSteamApi(SteamApi& api) {
    api.mod = LoadSteamApiDll();
    if (!api.mod) {
        fprintf(stderr,
            "Error: could not load steam_api64.dll.\n"
            "It should ship next to this tool.\n");
        return false;
    }

    bool ok = true;
    ok &= resolve(api.mod, "SteamAPI_Init", api.Init);
    ok &= resolve(api.mod, "SteamAPI_Shutdown", api.Shutdown);
    ok &= resolve(api.mod, "SteamAPI_RunCallbacks", api.RunCallbacks);
    ok &= resolve(api.mod, "SteamAPI_GetHSteamUser", api.GetHSteamUser);
    ok &= resolve(api.mod, "SteamAPI_GetHSteamPipe", api.GetHSteamPipe);
    ok &= resolve(api.mod, "SteamClient", api.SteamClient);
    ok &= resolve(api.mod, "SteamAPI_ISteamClient_GetISteamUGC", api.GetISteamUGC);
    ok &= resolve(api.mod, "SteamAPI_ISteamClient_GetISteamUser", api.GetISteamUser);
    ok &= resolve(api.mod, "SteamAPI_ISteamClient_GetISteamUtils", api.GetISteamUtils);
    ok &= resolve(api.mod, "SteamAPI_ISteamClient_GetISteamGenericInterface", api.GetGenericInterface);

    ok &= resolve(api.mod, "SteamAPI_ISteamUGC_CreateItem", api.CreateItem);
    ok &= resolve(api.mod, "SteamAPI_ISteamUGC_StartItemUpdate", api.StartItemUpdate);
    ok &= resolve(api.mod, "SteamAPI_ISteamUGC_SetItemTitle", api.SetItemTitle);
    ok &= resolve(api.mod, "SteamAPI_ISteamUGC_SetItemDescription", api.SetItemDescription);
    ok &= resolve(api.mod, "SteamAPI_ISteamUGC_SetItemVisibility", api.SetItemVisibility);
    ok &= resolve(api.mod, "SteamAPI_ISteamUGC_SetItemContent", api.SetItemContent);
    ok &= resolve(api.mod, "SteamAPI_ISteamUGC_SubmitItemUpdate", api.SubmitItemUpdate);
    ok &= resolve(api.mod, "SteamAPI_ISteamUGC_GetItemUpdateProgress", api.GetItemUpdateProgress);
    ok &= resolve(api.mod, "SteamAPI_ISteamUGC_CreateQueryUserUGCRequest", api.CreateQueryUserUGCRequest);
    ok &= resolve(api.mod, "SteamAPI_ISteamUGC_SendQueryUGCRequest", api.SendQueryUGCRequest);
    ok &= resolve(api.mod, "SteamAPI_ISteamUGC_ReleaseQueryUGCRequest", api.ReleaseQueryUGCRequest);
    ok &= resolve(api.mod, "SteamAPI_ISteamUGC_GetQueryUGCResult", api.GetQueryUGCResult);
    ok &= resolve(api.mod, "SteamAPI_ISteamUGC_SubscribeItem", api.SubscribeItem);
    ok &= resolve(api.mod, "SteamAPI_ISteamUGC_GetItemState", api.GetItemState);
    ok &= resolve(api.mod, "SteamAPI_ISteamUGC_GetItemInstallInfo", api.GetItemInstallInfo);
    ok &= resolve(api.mod, "SteamAPI_ISteamUGC_DownloadItem", api.DownloadItem);

    ok &= resolve(api.mod, "SteamAPI_ISteamUser_GetSteamID", api.GetSteamID);
    ok &= resolve(api.mod, "SteamAPI_ISteamUtils_GetAPICallResult", api.GetAPICallResult);
    return ok;
}

// Connect to the running Steam client as `appId`. Mirrors cloud760_tool.
static bool Connect(SteamApi& api, uint32_t appId) {
    char appIdStr[16];
    snprintf(appIdStr, sizeof(appIdStr), "%u", appId);
    SetEnvironmentVariableA("SteamAppId", appIdStr);
    SetEnvironmentVariableA("SteamAppID", appIdStr);
    SetEnvironmentVariableA("SteamGameId", appIdStr);

    bool init = api.Init();
    if (!init) {
        // Fallback: steam_appid.txt in the working directory.
        FILE* f = nullptr;
        if (fopen_s(&f, "steam_appid.txt", "wb") == 0 && f) {
            fwrite(appIdStr, 1, strlen(appIdStr), f);
            fclose(f);
            init = api.Init();
            DeleteFileW(L"steam_appid.txt");
        }
    }
    if (!init) {
        fprintf(stderr,
            "Error: SteamAPI_Init failed for AppID %u.\n"
            "Make sure Steam is running and you are logged in.\n", appId);
        return false;
    }

    void* client = api.SteamClient();
    if (!client) {
        fprintf(stderr, "Error: SteamClient() returned null.\n");
        return false;
    }
    HSteamUser hUser = api.GetHSteamUser();
    HSteamPipe hPipe = api.GetHSteamPipe();

    // Version fallback chains. The bundled 2021 steam_api64.dll ships
    // STEAMUGC_INTERFACE_VERSION015 / SteamUser021 / SteamUtils010, so those
    // come first; the wider ranges cover older and newer SDK builds.
    static const char* kUgcVersions[] = {
        "STEAMUGC_INTERFACE_VERSION015", "STEAMUGC_INTERFACE_VERSION014",
        "STEAMUGC_INTERFACE_VERSION016", "STEAMUGC_INTERFACE_VERSION017",
        "STEAMUGC_INTERFACE_VERSION018", "STEAMUGC_INTERFACE_VERSION013",
        "STEAMUGC_INTERFACE_VERSION012",
    };
    static const char* kUserVersions[] = {
        "SteamUser021", "SteamUser020", "SteamUser022", "SteamUser023",
        "SteamUser019", "SteamUser018",
    };
    static const char* kUtilsVersions[] = {
        "SteamUtils010", "SteamUtils009", "SteamUtils008",
    };

    // UGC: the specific flat thunk is unreliable in some bundled
    // steam_api64.dll builds (returns NULL for valid versions), so fall back
    // to the generic factory, which works for every interface.
    for (const char* v : kUgcVersions) {
        api.ugc = static_cast<ISteamUGC*>(api.GetISteamUGC(client, hUser, hPipe, v));
        if (api.ugc) break;
    }
    if (!api.ugc) {
        for (const char* v : kUgcVersions) {
            api.ugc = static_cast<ISteamUGC*>(api.GetGenericInterface(client, hUser, hPipe, v));
            if (api.ugc) break;
        }
    }
    // Utils: generic only; the flat GetISteamUtils thunk segfaults in the
    // bundled build.
    for (const char* v : kUtilsVersions) {
        api.utils = static_cast<ISteamUtils*>(api.GetGenericInterface(client, hUser, hPipe, v));
        if (api.utils) break;
    }
    for (const char* v : kUserVersions) {
        api.user = static_cast<ISteamUser*>(api.GetISteamUser(client, hUser, hPipe, v));
        if (api.user) break;
    }

    if (!api.ugc || !api.user || !api.utils) {
        fprintf(stderr, "Error: could not obtain ISteamUGC/ISteamUser/ISteamUtils.\n");
        return false;
    }
    return true;
}

// ── Item title derivation ──────────────────────────────────────────────
// Fixed project salt. The title is what a third party would need to guess to
// find an item (and items are Unlisted anyway), so HMAC-sha256 with a fixed
// salt is enough -- no secret to store, deterministic across machines.

static const char* kTitlePrefix = "CloudRedirect:";

static std::string ItemTitleFor(uint64_t steamid64, uint32_t gameAppid) {
    std::string msg = std::to_string(steamid64) + ":" + std::to_string(gameAppid);
    auto h = crypto::HmacSha256("CloudRedirect Workshop v1", msg);
    return std::string(kTitlePrefix) + crypto::ToHex(h.data(), h.size());
}

// ── Callback waiting ───────────────────────────────────────────────────

template <typename T>
static bool WaitForCallResult(SteamApi& api, SteamAPICall_t call, int callbackId,
                              T& out, int timeoutSec) {
    if (call == 0) return false;
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeoutSec * 1000;
    bool failed = false;
    for (;;) {
        if (api.GetAPICallResult(api.utils, call, &out, sizeof(out), callbackId, &failed))
            return !failed;
        if (GetTickCount64() >= deadline) {
            fprintf(stderr, "Error: timed out waiting for callback %d\n", callbackId);
            return false;
        }
        api.RunCallbacks();
        Sleep(50);
    }
}

// ── Item lookup by title ───────────────────────────────────────────────

// FindResult: 0 = not found (enumeration succeeded), 1 = found, -1 = error.
static constexpr int kFindNotFound = 0;
static constexpr int kFindFound = 1;
static constexpr int kFindError = -1;

// Enumerate the user's published items under the workshop appid; return the
// first whose title matches. Pages of 50 until the query is exhausted or a
// page cap is hit (5000 items is way beyond any realistic published count).
static int FindItemByTitle(SteamApi& api, AccountID_t accountId, AppId_t workshopAppid,
                           const std::string& title, PublishedFileId_t& outId,
                           SteamUGCDetails_t* outDetails) {
    for (uint32_t page = 1; page <= 100; ++page) {
        UGCQueryHandle_t query = api.CreateQueryUserUGCRequest(
            api.ugc, accountId, k_EUserUGCList_Published, k_EUGCMatchingUGCType_Items,
            k_EUserUGCListSortOrder_CreationOrderDesc, workshopAppid, workshopAppid, page);
        if (!query) {
            fprintf(stderr, "Error: CreateQueryUserUGCRequest failed\n");
            return kFindError;
        }

        SteamAPICall_t call = api.SendQueryUGCRequest(api.ugc, query);
        UGCQueryCompleted_t completed{};
        if (!WaitForCallResult(api, call, UGCQueryCompleted_t::k_iCallback, completed, 60)) {
            api.ReleaseQueryUGCRequest(api.ugc, query);
            return kFindError;
        }

        uint32_t count = 0;
        for (uint32_t i = 0; i < 1000; ++i) {
            SteamUGCDetails_t details{};
            if (!api.GetQueryUGCResult(api.ugc, query, i, &details)) break;
            ++count;
            if (title == details.m_rgchTitle) {
                outId = details.m_nPublishedFileId;
                if (outDetails) *outDetails = details;
                api.ReleaseQueryUGCRequest(api.ugc, query);
                return kFindFound;
            }
        }
        api.ReleaseQueryUGCRequest(api.ugc, query);
        if (count < 50) return kFindNotFound; // last page
    }
    return kFindNotFound;
}

// ── Push ───────────────────────────────────────────────────────────────

// Strip trailing separators: UGC rejects content paths with a trailing
// backslash, and the caller's command-line quoting would mangle them.
static std::string StripTrailingSeps(const std::string& path) {
    size_t end = path.size();
    while (end > 0 && (path[end - 1] == '\\' || path[end - 1] == '/')) --end;
    if (end == 0) return path;
    return path.substr(0, end);
}

// Long-path prefix. Game save trees (e.g. Grounded autosave names) exceed the
// legacy 260-char MAX_PATH; every Win32 call below goes through \\?\ so deep
// trees copy correctly.
static std::wstring Lp(const std::wstring& p) {
    if (p.size() >= 4 && p[0] == L'\\' && p[1] == L'\\' && p[2] == L'?' && p[3] == L'\\')
        return p;   // already prefixed
    if (p.size() >= 2 && p[1] == L':')
        return L"\\\\?\\" + p;   // absolute drive path
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\')
        return L"\\\\?\\UNC\\" + p.substr(2);  // UNC
    return p;   // relative — callers must not pass relative paths
}

// CreateDirectoryW only makes one level; a fresh mirror on a new machine has
// none of the intermediate dirs, so walk the chain (errors on existing dirs
// are fine and ignored).
static void CreateDirectoryTree(const std::wstring& dir) {
    // Skip the drive prefix ("M:\") — creating it is invalid.
    size_t start = (dir.size() >= 3 && dir[1] == L':' && dir[2] == L'\\') ? 3 : 0;
    for (size_t i = start; i < dir.size(); ++i) {
        if (dir[i] == L'\\')
            CreateDirectoryW(Lp(dir.substr(0, i)).c_str(), nullptr);
    }
    CreateDirectoryW(Lp(dir).c_str(), nullptr);
}

// SetItemContent's path handling in the bundled steam_api64.dll cannot deal
// with non-ASCII paths (UTF-8 bytes yield FileNotFound from SubmitItemUpdate,
// ACP bytes yield Fail). Stage the tree into a fresh ASCII temp dir instead
// and upload from there; wide APIs make the copy itself encoding-correct.
static bool HasNonAscii(const std::string& s) {
    for (unsigned char c : s)
        if (c > 0x7F) return true;
    return false;
}

static std::wstring PickAsciiTempBase() {
    wchar_t tmp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tmp) && !HasNonAscii(ToUtf8(tmp)))
        return tmp;
    // %TEMP% itself is non-ASCII (localized user profile): C:\Windows\Temp
    // is a fixed English name on disk on every Windows locale.
    return L"C:\\Windows\\Temp\\";
}

static bool CopyTreeToStaging(const std::wstring& srcDir, const std::wstring& dstDir) {
    WIN32_FIND_DATAW fd{};
    std::wstring pattern = Lp(srcDir) + L"\\*";
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return true;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring src = srcDir + L"\\" + fd.cFileName;
        std::wstring dst = dstDir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CreateDirectoryW(Lp(dst).c_str(), nullptr);
            if (!CopyTreeToStaging(src, dst)) {
                FindClose(hFind);
                return false;
            }
            continue;
        }
        if (!CopyFileW(Lp(src).c_str(), Lp(dst).c_str(), FALSE)) {
            fprintf(stderr, "Error: staging copy %s failed (err %lu)\n",
                    ToUtf8(src).c_str(), GetLastError());
            FindClose(hFind);
            return false;
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    return true;
}

static void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    std::wstring pattern = Lp(dir) + L"\\*";
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring child = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            RemoveTree(child);
        else
            DeleteFileW(Lp(child).c_str());
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    RemoveDirectoryW(Lp(dir).c_str());
}

// Exit codes (shared with the provider that spawns this tool).
enum ToolExit {
    EXIT_OK = 0, EXIT_ERROR = 1, EXIT_AGREEMENT = 2,
    EXIT_NOT_LOGGED_IN = 3, EXIT_RATELIMIT = 4, EXIT_ACCOUNT_MISMATCH = 5,
};

static int CmdPush(SteamApi& api, AppId_t workshopAppid, uint32_t gameAppid,
                   uint32_t expectedAccount, int visibility,
                   const std::string& contentDirArg, uint64_t steamid64) {
    const std::string contentDir = StripTrailingSeps(contentDirArg);
    if (expectedAccount != 0 && (steamid64 & 0xFFFFFFFFu) != expectedAccount) {
        fprintf(stderr, "ACCOUNTMISMATCH expected=%u actual=%llu\n",
                expectedAccount, (unsigned long long)(steamid64 & 0xFFFFFFFFu));
        return EXIT_ACCOUNT_MISMATCH;
    }

    std::string title = ItemTitleFor(steamid64, gameAppid);
    PublishedFileId_t itemId = 0;
    int found = FindItemByTitle(api, (AccountID_t)(steamid64 & 0xFFFFFFFFu),
                                workshopAppid, title, itemId, nullptr);
    if (found == kFindError) {
        fprintf(stderr, "Error: could not enumerate published items\n");
        return EXIT_ERROR;
    }

    if (itemId == 0) {
        SteamAPICall_t call = api.CreateItem(api.ugc, workshopAppid, k_EWorkshopFileTypeCommunity);
        CreateItemResult_t created{};
        if (!WaitForCallResult(api, call, CreateItemResult_t::k_iCallback, created, 120)) {
            fprintf(stderr, "Error: CreateItem timed out\n");
            return EXIT_ERROR;
        }
        if (created.m_bUserNeedsToAcceptWorkshopLegalAgreement) {
            fprintf(stderr, "AGREEMENT: the Steam Workshop legal agreement must be accepted first\n");
            return EXIT_AGREEMENT;
        }
        if (created.m_eResult != k_EResultOK || created.m_nPublishedFileId == 0) {
            fprintf(stderr, "Error: CreateItem failed, EResult=%d agree=%d id=%llu\n",
                    created.m_eResult, (int)created.m_bUserNeedsToAcceptWorkshopLegalAgreement,
                    (unsigned long long)created.m_nPublishedFileId);
            return EXIT_ERROR;
        }
        itemId = created.m_nPublishedFileId;
        fprintf(stdout, "CREATED %llu\n", (unsigned long long)itemId);
    }

    UGCUpdateHandle_t handle = api.StartItemUpdate(api.ugc, workshopAppid, itemId);
    if (handle == 0xFFFFFFFFFFFFFFFFull || handle == 0) {
        fprintf(stderr, "Error: StartItemUpdate failed\n");
        return EXIT_ERROR;
    }

    // Title + visibility are set on every update: cheap, and repairs an item
    // whose metadata got mangled.
    if (!api.SetItemTitle(api.ugc, handle, title.c_str())) {
        fprintf(stderr, "Error: SetItemTitle failed\n");
        return EXIT_ERROR;
    }
    if (!api.SetItemDescription(api.ugc, handle,
            "CloudRedirect cloud save data for one game. Do not delete.")) {
        fprintf(stderr, "Error: SetItemDescription failed\n");
        return EXIT_ERROR;
    }
    if (!api.SetItemVisibility(api.ugc, handle, visibility)) {
        fprintf(stderr, "Error: SetItemVisibility failed\n");
        return EXIT_ERROR;
    }

    // SetItemContent requires an existing folder. A delete-only sync can leave
    // the content dir missing, so fall back to a fresh empty temp dir.
    // Non-ASCII content paths are staged into an ASCII temp dir (see
    // HasNonAscii) because this steam_api64.dll cannot handle them.
    std::wstring cleanupDir;
    std::string cleanupDirUtf8;
    const char* contentPath = contentDir.c_str();
    if (GetFileAttributesW(Lp(ToWide(contentDir)).c_str()) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "Warning: content dir %s missing; uploading empty content\n",
                contentDir.c_str());
        std::wstring base = PickAsciiTempBase();
        cleanupDir = base + L"crws_empty_" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(Lp(cleanupDir).c_str(), nullptr);
        cleanupDirUtf8 = ToUtf8(cleanupDir);
        contentPath = cleanupDirUtf8.c_str();
    } else if (HasNonAscii(contentDir) || contentDir.size() > 200) {
        // Non-ASCII paths cannot be staged by Steam's UGC packer (see
        // HasNonAscii), and very long trees risk MAX_PATH issues inside the
        // client; stage both into a short ASCII temp dir instead.
        std::wstring base = PickAsciiTempBase();
        cleanupDir = base + L"crws_stage_" + std::to_wstring(GetCurrentProcessId()) +
            L"_" + std::to_wstring(GetTickCount64());
        CreateDirectoryW(Lp(cleanupDir).c_str(), nullptr);
        if (!CopyTreeToStaging(ToWide(contentDir), cleanupDir)) {
            fprintf(stderr, "Error: staging %s into an ASCII temp dir failed\n",
                    contentDir.c_str());
            RemoveTree(cleanupDir);
            return EXIT_ERROR;
        }
        cleanupDirUtf8 = ToUtf8(cleanupDir);
        contentPath = cleanupDirUtf8.c_str();
        fprintf(stderr, "DBG: content path staged to %s\n", contentPath);
    }

    if (!api.SetItemContent(api.ugc, handle, contentPath)) {
        fprintf(stderr, "Error: SetItemContent failed for %s\n", contentPath);
        if (!cleanupDir.empty()) RemoveTree(cleanupDir);
        return EXIT_ERROR;
    }

    char note[64];
    snprintf(note, sizeof(note), "CloudRedirect sync (game %u)", gameAppid);
    SteamAPICall_t call = api.SubmitItemUpdate(api.ugc, handle, note);
    SubmitItemUpdateResult_t submitted{};
    if (!WaitForCallResult(api, call, SubmitItemUpdateResult_t::k_iCallback, submitted, 1800)) {
        fprintf(stderr, "Error: SubmitItemUpdate timed out\n");
        return EXIT_ERROR;
    }

    if (!cleanupDir.empty()) RemoveTree(cleanupDir);

    fprintf(stderr, "DBG: submit result eResult=%d agree=%d id=%llu item=%llu content=%s\n",
            submitted.m_eResult, (int)submitted.m_bUserNeedsToAcceptWorkshopLegalAgreement,
            (unsigned long long)submitted.m_nPublishedFileId,
            (unsigned long long)itemId, contentPath);

    if (submitted.m_bUserNeedsToAcceptWorkshopLegalAgreement) {
        fprintf(stderr, "AGREEMENT: the Steam Workshop legal agreement must be accepted first\n");
        return EXIT_AGREEMENT;
    }
    if (submitted.m_eResult == k_EResultRateLimitExceeded || submitted.m_eResult == k_EResultBusy) {
        fprintf(stderr, "RATELIMIT: EResult=%d, retry later\n", submitted.m_eResult);
        return EXIT_RATELIMIT;
    }
    if (submitted.m_eResult == k_EResultNotLoggedOn) {
        fprintf(stderr, "NOTLOGGEDIN\n");
        return EXIT_NOT_LOGGED_IN;
    }
    if (submitted.m_eResult != k_EResultOK) {
        if (submitted.m_eResult == k_EResultLimitExceeded) {
            fprintf(stderr,
                "Error: item size/limit exceeded (EResult=%d). Steam Workshop items "
                "have a per-item size cap; the save data for this game is too large.\n",
                submitted.m_eResult);
        } else if (submitted.m_eResult == k_EResultFileNotFound) {
            fprintf(stderr,
                "Error: SubmitItemUpdate EResult=FileNotFound -- the content folder %s "
                "could not be staged by Steam\n", contentPath);
        } else {
            fprintf(stderr, "Error: SubmitItemUpdate failed, EResult=%d\n", submitted.m_eResult);
        }
        return EXIT_ERROR;
    }

    fprintf(stdout, "ITEM %llu\n", (unsigned long long)itemId);
    return EXIT_OK;
}

// ── Pull ───────────────────────────────────────────────────────────────

// Copy files from srcDir into dstDir, keeping whichever is newer per file.
// Copies go through a temp name + rename so readers never see partial files.
static bool CopyNewerTree(const std::wstring& srcDir, const std::wstring& dstDir) {
    WIN32_FIND_DATAW fd{};
    std::wstring pattern = Lp(srcDir) + L"\\*";
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return true;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring src = srcDir + L"\\" + fd.cFileName;
        std::wstring dst = dstDir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CreateDirectoryTree(dst);
            if (!CopyNewerTree(src, dst)) {
                FindClose(hFind);
                return false;
            }
            continue;
        }

        bool copy = true;
        WIN32_FIND_DATAW dstFd{};
        HANDLE hDst = FindFirstFileW(Lp(dst).c_str(), &dstFd);
        if (hDst != INVALID_HANDLE_VALUE) {
            // Same size + not newer remote -> skip.
            if (dstFd.nFileSizeHigh == fd.nFileSizeHigh &&
                dstFd.nFileSizeLow == fd.nFileSizeLow &&
                CompareFileTime(&dstFd.ftLastWriteTime, &fd.ftLastWriteTime) >= 0) {
                copy = false;
            }
            FindClose(hDst);
        }

        if (!copy) continue;

        std::wstring tmp = dst + L".crws.tmp";
        DeleteFileW(Lp(tmp).c_str());
        if (!CopyFileW(Lp(src).c_str(), Lp(tmp).c_str(), FALSE)) {
            fprintf(stderr, "Error: copy %s -> %s failed (err %lu)\n",
                    ToUtf8(src).c_str(), ToUtf8(dst).c_str(), GetLastError());
            FindClose(hFind);
            return false;
        }
        if (!MoveFileExW(Lp(tmp).c_str(), Lp(dst).c_str(), MOVEFILE_REPLACE_EXISTING)) {
            fprintf(stderr, "Error: rename %s -> %s failed (err %lu)\n",
                    ToUtf8(tmp).c_str(), ToUtf8(dst).c_str(), GetLastError());
            FindClose(hFind);
            return false;
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    return true;
}

static int CmdPull(SteamApi& api, AppId_t workshopAppid, uint32_t gameAppid,
                   uint32_t expectedAccount, const std::string& contentDirArg,
                   int timeoutSec, uint64_t steamid64) {
    const std::string contentDir = StripTrailingSeps(contentDirArg);
    if (expectedAccount != 0 && (steamid64 & 0xFFFFFFFFu) != expectedAccount) {
        fprintf(stderr, "ACCOUNTMISMATCH expected=%u actual=%llu\n",
                expectedAccount, (unsigned long long)(steamid64 & 0xFFFFFFFFu));
        return EXIT_ACCOUNT_MISMATCH;
    }

    std::string title = ItemTitleFor(steamid64, gameAppid);
    PublishedFileId_t itemId = 0;
    SteamUGCDetails_t details{};
    int found = FindItemByTitle(api, (AccountID_t)(steamid64 & 0xFFFFFFFFu),
                                workshopAppid, title, itemId, &details);
    if (found == kFindError) {
        fprintf(stderr, "Error: could not enumerate published items\n");
        return EXIT_ERROR;
    }
    if (itemId == 0) {
        fprintf(stdout, "NONE\n");
        return EXIT_OK;
    }

    // Subscribe (idempotent) and download until installed AND up to date.
    // k_EItemStateNeedsUpdate matters: another machine may have pushed a new
    // item revision that Steam has not applied yet; merging the stale
    // installed content then would lose the other machine's saves.
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeoutSec * 1000;
    uint32_t state = api.GetItemState(api.ugc, itemId);
    if (!(state & k_EItemStateSubscribed)) {
        api.SubscribeItem(api.ugc, itemId);
        state = api.GetItemState(api.ugc, itemId);
    }
    if (!(state & k_EItemStateInstalled) || (state & k_EItemStateNeedsUpdate)) {
        api.DownloadItem(api.ugc, itemId, true);
        for (;;) {
            api.RunCallbacks();
            Sleep(100);
            state = api.GetItemState(api.ugc, itemId);
            if ((state & k_EItemStateInstalled) && !(state & k_EItemStateNeedsUpdate))
                break;
            if (GetTickCount64() >= deadline) {
                fprintf(stderr, "Error: timed out waiting for item %llu download\n",
                        (unsigned long long)itemId);
                return EXIT_ERROR;
            }
        }
    }

    // Even with a clean state, the client can briefly serve stale content
    // while a just-pushed item update propagates (no NeedsUpdate flag yet).
    // Compare the on-disk install timestamp with the item's server-side
    // update time and keep waiting until the local copy is current -- merging
    // a stale tree would silently drop another machine's newest saves.
    if (details.m_rtimeUpdated > 0) {
        for (;;) {
            uint64_t sizeOnDisk = 0;
            uint32_t ts = 0;
            char tmpFolder[MAX_PATH] = {};
            api.GetItemInstallInfo(api.ugc, itemId, &sizeOnDisk,
                                   tmpFolder, sizeof(tmpFolder), &ts);
            if (ts > 0 && (uint64_t)ts >= details.m_rtimeUpdated) break;
            api.DownloadItem(api.ugc, itemId, true);
            Sleep(500);
            if (GetTickCount64() >= deadline) {
                fprintf(stderr, "Error: timed out waiting for item %llu content to refresh\n",
                        (unsigned long long)itemId);
                return EXIT_ERROR;
            }
        }
    }

    char folder[MAX_PATH * 2] = {};
    uint64_t sizeOnDisk = 0;
    uint32_t timestamp = 0;
    if (!api.GetItemInstallInfo(api.ugc, itemId, &sizeOnDisk,
                                folder, sizeof(folder), &timestamp) || !folder[0]) {
        fprintf(stderr, "Error: GetItemInstallInfo failed for item %llu\n",
                (unsigned long long)itemId);
        return EXIT_ERROR;
    }

    std::wstring srcWide = ToWideTolerant(folder);
    std::wstring dstWide = ToWide(contentDir);
    if (srcWide.empty() || dstWide.empty()) {
        fprintf(stderr, "Error: bad paths for merge\n");
        return EXIT_ERROR;
    }
    CreateDirectoryTree(dstWide);
    if (!CopyNewerTree(srcWide, dstWide)) {
        fprintf(stderr, "Error: merging item content into %s failed\n", contentDir.c_str());
        return EXIT_ERROR;
    }

    fprintf(stdout, "ITEM %llu\n", (unsigned long long)itemId);
    return EXIT_OK;
}

// ── Find / list ────────────────────────────────────────────────────────

static int CmdFind(SteamApi& api, AppId_t workshopAppid, uint32_t gameAppid, uint64_t steamid64) {
    std::string title = ItemTitleFor(steamid64, gameAppid);
    PublishedFileId_t itemId = 0;
    int found = FindItemByTitle(api, (AccountID_t)(steamid64 & 0xFFFFFFFFu),
                                workshopAppid, title, itemId, nullptr);
    if (found == kFindError) {
        fprintf(stderr, "Error: could not enumerate published items\n");
        return EXIT_ERROR;
    }
    if (itemId == 0) {
        fprintf(stdout, "NONE\n");
        return EXIT_OK;
    }
    fprintf(stdout, "ITEM %llu\n", (unsigned long long)itemId);
    fprintf(stdout, "URL https://steamcommunity.com/sharedfiles/filedetails/?id=%llu\n",
            (unsigned long long)itemId);
    return EXIT_OK;
}

static int CmdList(SteamApi& api, AppId_t workshopAppid, uint64_t steamid64) {
    AccountID_t accountId = (AccountID_t)(steamid64 & 0xFFFFFFFFu);
    for (uint32_t page = 1; page <= 100; ++page) {
        UGCQueryHandle_t query = api.CreateQueryUserUGCRequest(
            api.ugc, accountId, k_EUserUGCList_Published, k_EUGCMatchingUGCType_Items,
            k_EUserUGCListSortOrder_CreationOrderDesc, workshopAppid, workshopAppid, page);
        if (!query) {
            fprintf(stderr, "Error: CreateQueryUserUGCRequest failed\n");
            return EXIT_ERROR;
        }
        SteamAPICall_t call = api.SendQueryUGCRequest(api.ugc, query);
        UGCQueryCompleted_t completed{};
        if (!WaitForCallResult(api, call, UGCQueryCompleted_t::k_iCallback, completed, 60)) {
            api.ReleaseQueryUGCRequest(api.ugc, query);
            return EXIT_ERROR;
        }
        uint32_t count = 0;
        for (uint32_t i = 0; i < 1000; ++i) {
            SteamUGCDetails_t details{};
            if (!api.GetQueryUGCResult(api.ugc, query, i, &details)) break;
            ++count;
            if (strncmp(details.m_rgchTitle, kTitlePrefix, strlen(kTitlePrefix)) == 0) {
                fprintf(stdout, "ITEM %llu\t%s\n",
                        (unsigned long long)details.m_nPublishedFileId, details.m_rgchTitle);
            }
        }
        api.ReleaseQueryUGCRequest(api.ugc, query);
        if (count < 50) return EXIT_OK;
    }
    return EXIT_OK;
}

// ── Main ───────────────────────────────────────────────────────────────

static bool ParseU32(const std::string& s, uint32_t& out) {
    char* end = nullptr;
    unsigned long v = strtoul(s.c_str(), &end, 10);
    if (!end || *end != '\0' || v == 0 || v > 0xFFFFFFFFul) return false;
    out = (uint32_t)v;
    return true;
}

// --game-appid may be 0: account-scope items (stats.json etc.) keyed by the
// user alone.
static bool ParseU32ZeroOk(const std::string& s, uint32_t& out) {
    char* end = nullptr;
    unsigned long v = strtoul(s.c_str(), &end, 10);
    if (!end || *end != '\0' || v > 0xFFFFFFFFul) return false;
    out = (uint32_t)v;
    return true;
}

static bool ParseI32(const std::string& s, int32_t& out) {
    char* end = nullptr;
    long v = strtol(s.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    out = (int32_t)v;
    return true;
}

struct Options {
    std::string command;
    std::string content;
    uint32_t gameAppid = 0;
    uint32_t workshopAppid = 480;   // Spacewar, like the original tool
    uint32_t expectedAccount = 0;
    int32_t visibility = k_ERemoteStoragePublishedFileVisibilityUnlisted;
    int32_t timeoutSec = 600;
    bool ok = true;
};

static Options ParseArgs(const std::vector<std::string>& args) {
    Options o;
    if (args.size() < 2) { o.ok = false; return o; }
    o.command = args[1];
    for (size_t i = 2; i < args.size(); ++i) {
        std::string a = args[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= args.size()) { fprintf(stderr, "Error: %s needs a value\n", name); o.ok = false; return {}; }
            return args[++i];
        };
        if (a == "--content")       o.content = next("--content");
        else if (a == "--game-appid")     { uint32_t v; if (!ParseU32ZeroOk(next("--game-appid"), v)) o.ok = false; else o.gameAppid = v; }
        else if (a == "--workshop-appid") { uint32_t v; if (!ParseU32(next("--workshop-appid"), v)) o.ok = false; else o.workshopAppid = v; }
        else if (a == "--account")       { uint32_t v; if (!ParseU32(next("--account"), v)) o.ok = false; else o.expectedAccount = v; }
        else if (a == "--visibility")    { int32_t v; if (!ParseI32(next("--visibility"), v)) o.ok = false; else o.visibility = v; }
        else if (a == "--timeout-sec")   { int32_t v; if (!ParseI32(next("--timeout-sec"), v)) o.ok = false; else o.timeoutSec = v; }
        else { fprintf(stderr, "Error: unknown option %s\n", a.c_str()); o.ok = false; }
    }

    if (o.command == "push" || o.command == "pull") {
        if (o.content.empty()) { fprintf(stderr, "Error: --content is required\n"); o.ok = false; }
    }
    if (o.visibility < 0 || o.visibility > 3) {
        fprintf(stderr, "Error: --visibility must be 0..3\n");
        o.ok = false;
    }
    return o;
}

static void Usage() {
    fprintf(stderr,
        "workshop_sync_tool - push/pull CloudRedirect saves to/from a Steam Workshop item\n\n"
        "Usage:\n"
        "  workshop_sync_tool push --content <dir> --game-appid <n> [--workshop-appid <n>] [--visibility 0-3] [--account <acct>]\n"
        "  workshop_sync_tool pull --content <dir> --game-appid <n> [--workshop-appid <n>] [--account <acct>] [--timeout-sec <n>]\n"
        "  workshop_sync_tool find --game-appid <n> [--workshop-appid <n>]\n"
        "  workshop_sync_tool list [--workshop-appid <n>]\n\n"
        "Steam must be running and logged in. steam_api64.dll must be next to this exe.\n"
        "Exit codes: 0 ok, 2 workshop agreement needed, 3 not logged in,\n"
        "            4 rate limited, 5 account mismatch, 1 error.\n");
}

int wmain(int argc, wchar_t** argv) {
    // stderr is fully buffered when piped, which hides crash locations.
    setvbuf(stderr, nullptr, _IONBF, 0);

    // Lossless UTF-8 arguments: main() would round-trip the command line
    // through the ANSI code page and corrupt non-ASCII (Chinese) paths.
    std::vector<std::string> u8Args;
    u8Args.reserve((size_t)argc);
    for (int i = 0; i < argc; ++i)
        u8Args.push_back(ToUtf8(argv[i]));

    Options o = ParseArgs(u8Args);
    if (!o.ok) { Usage(); return 1; }

    SteamApi api;
    if (!LoadSteamApi(api)) return 1;
    if (!Connect(api, o.workshopAppid)) { api.Shutdown(); return EXIT_NOT_LOGGED_IN; }

    uint64_t steamid64 = api.GetSteamID(api.user);
    if (steamid64 == 0) {
        fprintf(stderr, "Error: GetSteamID returned 0\n");
        api.Shutdown();
        return EXIT_ERROR;
    }

    int rc = EXIT_ERROR;
    if (o.command == "push")
        rc = CmdPush(api, o.workshopAppid, o.gameAppid, o.expectedAccount, o.visibility, o.content, steamid64);
    else if (o.command == "pull")
        rc = CmdPull(api, o.workshopAppid, o.gameAppid, o.expectedAccount, o.content, o.timeoutSec, steamid64);
    else if (o.command == "find")
        rc = CmdFind(api, o.workshopAppid, o.gameAppid, steamid64);
    else if (o.command == "list")
        rc = CmdList(api, o.workshopAppid, steamid64);
    else { Usage(); rc = EXIT_ERROR; }

    api.Shutdown();
    return rc;
}
