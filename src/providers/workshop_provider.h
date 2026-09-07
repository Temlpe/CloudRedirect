#pragma once
#include "cloud_provider.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Steam Workshop provider.
//
// The cloud namespace ("{accountId}/{appId}/...") is mirrored into a local
// workspace directory. Each (accountId, appId) subtree is synced to its own
// Steam Workshop item, published under a configurable host AppID (default 480,
// Spacewar) and identified by a title derived from an HMAC of
// (steamid64, gameAppid) -- see workshop_sync_tool.cpp. That makes the item
// findable from any machine without storing its id anywhere.
//
// The actual Steam UGC calls happen in a separate process
// (workshop_sync_tool.exe) because this DLL runs inside the Steam client,
// whose own AppID has no Workshop support. The provider spawns the tool for
// pulls (blocking, once per (accountId, appId) per session, before the first
// access) and pushes (background, debounced, after Upload/Remove).
//
// Windows only: on other platforms Init fails and the host falls back to
// local-only mode.

class WorkshopProvider : public ICloudProvider {
public:
    using NotifyFn = std::function<void(const std::string& message)>;

    const char* Name() const override { return "Steam Workshop"; }

    // configPath is either the raw content of config.json (the host DLL passes
    // it so the provider can read sync_path / workshop_* keys directly) or a
    // filesystem path whose file content is that JSON, or -- when neither --
    // a workspace directory path used with default settings.
    bool Init(const std::string& configPath) override;
    void Shutdown() override;
    bool IsAuthenticated() const override { return true; }

    bool Upload(const std::string& path, const uint8_t* data, size_t len) override;
    bool Download(const std::string& path, std::vector<uint8_t>& outData) override;
    bool Remove(const std::string& path) override;
    ExistsStatus CheckExists(const std::string& path) override;
    std::vector<FileInfo> List(const std::string& prefix) override;
    bool ListChecked(const std::string& prefix, std::vector<FileInfo>& outFiles,
                     bool* outComplete = nullptr) override;

    // User-visible problem reporter (workshop legal agreement, Steam offline,
    // item size limits). Wired by the host; unset = log only.
    void SetNotifyFn(NotifyFn fn) { m_notifyFn = std::move(fn); }

private:
    struct ItemState {
        std::mutex mtx;
        std::condition_variable cv;
        bool pullDone = false;         // pull completed successfully this session
        bool pullInFlight = false;
        bool dirty = false;            // local changes not pushed yet
        bool pushInFlight = false;
        int pullAttempts = 0;          // failed pull attempts this session
        std::chrono::steady_clock::time_point dirtySince{};
        std::chrono::steady_clock::time_point pushRetryAfter{};
        std::chrono::steady_clock::time_point lastPullAttempt{};
        std::chrono::steady_clock::time_point pullFreshUntil{}; // mirror is fresh until then
        uint64_t itemId = 0;
        // relPath (relative to the item root) -> unix ts of the local deletion.
        // Pull merge cannot resurrect these; cleared after a successful push.
        std::map<std::string, uint64_t> tombstones;
    };

    // ── Configuration ──────────────────────────────────────────────────
    std::string m_root;                 // workspace root, ends with separator
    std::string m_toolPath;             // workshop_sync_tool.exe
    uint32_t m_workshopAppid = 480;
    int32_t m_visibility = 3;           // unlisted
    int m_pushDelaySec = 90;
    int m_pullTimeoutSec = 120;
    int m_pullFreshnessSec = 600;       // mirror considered fresh for this long
    bool m_initialized = false;

    // ── State ──────────────────────────────────────────────────────────
    std::mutex m_registryMtx;
    std::map<uint64_t, std::shared_ptr<ItemState>> m_items;
    std::mutex m_toolMutex;             // one workshop_sync_tool.exe at a time
    std::mutex m_activeProcessMtx;      // guards m_activeToolProcess
    void* m_activeToolProcess = nullptr; // running tool's process handle (kill at shutdown)
    std::atomic<bool> m_notifiedMissingTool{false}; // one "tool missing" notice per session
    std::thread m_pushThread;
    std::thread m_prefetchThread;
    std::mutex m_pushMtx;
    std::condition_variable m_pushCv;
    std::atomic<bool> m_shutdown{false};
    std::atomic<uint32_t> m_currentAccountId{0}; // detected from first tool run
    NotifyFn m_notifyFn;

    // ── Mirror filesystem helpers (same containment rules as LocalDisk) ─
    std::string ToFullPath(const std::string& relPath) const;
    std::string ItemRootDir(uint32_t accountId, uint32_t appId) const;
    std::string MetaDir() const;

    // ── Per-item logic ─────────────────────────────────────────────────
    static bool ParseItemPath(const std::string& path, uint32_t& accountId,
                              uint32_t& appId);
    std::shared_ptr<ItemState> GetItemState(uint32_t accountId, uint32_t appId);
    void EnsurePulled(uint32_t accountId, uint32_t appId);
    void MarkDirty(uint32_t accountId, uint32_t appId);
    void LoadMeta(const std::shared_ptr<ItemState>& st, uint32_t accountId, uint32_t appId);
    void SaveMeta(const std::shared_ptr<ItemState>& st, uint32_t accountId, uint32_t appId);
    void WriteDirtyFlag(uint32_t accountId, uint32_t appId);
    void ClearDirtyFlag(uint32_t accountId, uint32_t appId);

    // ── Worker tool interaction ────────────────────────────────────────
    bool EnsureToolExtracted();
    int RunTool(const std::vector<std::string>& args, std::string& stdoutOut,
                int timeoutSec);
    int PushItem(uint32_t accountId, uint32_t appId,
                 const std::shared_ptr<ItemState>& st);
    int PullItem(uint32_t accountId, uint32_t appId,
                 const std::shared_ptr<ItemState>& st);
    void PushLoop();
    void Notify(const std::string& message);
};
