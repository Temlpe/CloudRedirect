#include "workshop_provider.h"

#include "file_util.h"
#include "json.h"
#include "log.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ShlObj.h>
#endif

namespace {

// workshop_sync_tool.exe exit codes (see workshop_sync_tool.cpp).
constexpr int kToolAgreement = 2;
constexpr int kToolNotLoggedIn = 3;
constexpr int kToolRateLimit = 4;
constexpr int kToolAccountMismatch = 5;

// Retry delays for failed background pushes.
constexpr int kRetryAfterAgreementSec = 600;
constexpr int kRetryAfterRateLimitSec = 900;
constexpr int kRetryAfterOtherSec = 300;

// Tool gets its own generous internal timeout; the spawn wait adds margin.
constexpr int kPushTimeoutSec = 1800;
constexpr int kPushWaitSec = kPushTimeoutSec + 60;
constexpr int kPullWaitMarginSec = 60;

// Pull failure backoff: a failed pull (Steam starting up, offline) must not
// poison the whole session -- a new machine would start the game with no
// saves. Retry with backoff up to a per-session cap.
constexpr int kPullRetryBackoffSec = 30;
constexpr int kMaxPullAttempts = 10;
constexpr int kPullInlineRetryDelaySec = 5;

// Mirror freshness window: a mirror older than this is re-pulled on the next
// access (and always before a push), so saves uploaded from another machine
// reach this one within the window instead of only at the next Steam restart.
constexpr int kPullFreshnessSecDefault = 600;

#ifdef _WIN32
// Roaming AppData without SHGetKnownFolderPath's pointer dance.
std::string GetAppDataDir() {
    PWSTR wide = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT,
                                      nullptr, &wide);
    if (FAILED(hr) || !wide) return {};
    std::string utf8 = FileUtil::WideToUtf8(wide);
    CoTaskMemFree(wide);
    return utf8;
}
#endif

} // namespace

// ── Init / Shutdown ────────────────────────────────────────────────────

bool WorkshopProvider::Init(const std::string& configPath) {
#ifndef _WIN32
    (void)configPath;
    LOG("[WorkshopProvider] Steam Workshop provider is Windows-only; refusing init");
    return false;
#else
    // configPath may be: raw config.json content, a path to a JSON file, or a
    // plain workspace directory.
    std::string jsonText;
    if (!configPath.empty() && configPath.front() == '{') {
        jsonText = configPath;
    } else {
        std::ifstream f(FileUtil::Utf8ToPath(configPath), std::ios::binary);
        if (f) {
            jsonText.assign(std::istreambuf_iterator<char>(f), {});
        } else if (!configPath.empty()) {
            // Not a readable file: treat as a workspace directory directly,
            // unless it looks like a missing JSON config (then use defaults).
            size_t lastSlash = configPath.find_last_of("\\/");
            std::string tail = configPath.substr(
                lastSlash == std::string::npos ? 0 : lastSlash + 1);
            std::string lower = tail;
            for (auto& c : lower) c = (char)tolower((unsigned char)c);
            if (lower.size() < 5 || lower.compare(lower.size() - 5, 5, ".json") != 0)
                m_root = configPath;
        }
    }

    std::string workspaceDir;
    if (!jsonText.empty()) {
        auto cfg = Json::Parse(jsonText);
        // sync_path is the canonical workspace key (same field the folder
        // provider uses); workspace_dir is accepted as an alias.
        if (cfg["sync_path"].type == Json::Type::String && !cfg["sync_path"].str().empty())
            workspaceDir = cfg["sync_path"].str();
        else if (cfg["workspace_dir"].type == Json::Type::String && !cfg["workspace_dir"].str().empty())
            workspaceDir = cfg["workspace_dir"].str();
        if (cfg["workshop_appid"].type == Json::Type::Number && cfg["workshop_appid"].integer() > 0)
            m_workshopAppid = (uint32_t)cfg["workshop_appid"].integer();
        if (cfg["workshop_visibility"].type == Json::Type::Number) {
            int64_t v = cfg["workshop_visibility"].integer();
            if (v >= 0 && v <= 3) m_visibility = (int32_t)v;
        }
        if (cfg["workshop_push_delay_sec"].type == Json::Type::Number &&
            cfg["workshop_push_delay_sec"].integer() >= 0)
            m_pushDelaySec = (int)cfg["workshop_push_delay_sec"].integer();
        if (cfg["workshop_pull_timeout_sec"].type == Json::Type::Number &&
            cfg["workshop_pull_timeout_sec"].integer() >= 10)
            m_pullTimeoutSec = (int)cfg["workshop_pull_timeout_sec"].integer();
        if (cfg["workshop_pull_fresh_sec"].type == Json::Type::Number &&
            cfg["workshop_pull_fresh_sec"].integer() >= 60)
            m_pullFreshnessSec = (int)cfg["workshop_pull_fresh_sec"].integer();
        if (cfg["workshop_tool_path"].type == Json::Type::String && !cfg["workshop_tool_path"].str().empty())
            m_toolPath = cfg["workshop_tool_path"].str();
    }

    if (workspaceDir.empty() && m_root.empty()) {
        std::string appData = GetAppDataDir();
        if (appData.empty()) {
            LOG("[WorkshopProvider] Could not resolve %%APPDATA%% for the default workspace");
            return false;
        }
        workspaceDir = appData + "\\CloudRedirect\\workshop_mirror";
    }

    if (m_toolPath.empty()) {
        std::string appData = GetAppDataDir();
        if (appData.empty()) {
            LOG("[WorkshopProvider] Could not resolve %%APPDATA%% for the default tool path");
            return false;
        }
        m_toolPath = appData + "\\CloudRedirect\\tools\\workshop_sync_tool.exe";
    }

    if (m_root.empty()) m_root = workspaceDir;
    if (!m_root.empty() && m_root.back() != '\\' && m_root.back() != '/')
        m_root += '\\';

    std::error_code ec;
    std::filesystem::create_directories(FileUtil::LongPath(FileUtil::Utf8ToPath(m_root)), ec);
    std::filesystem::create_directories(FileUtil::LongPath(FileUtil::Utf8ToPath(MetaDir())), ec);
    if (ec) {
        LOG("[WorkshopProvider] Failed to create workspace %s: %s",
            m_root.c_str(), ec.message().c_str());
        return false;
    }

    m_initialized = true;
    m_shutdown.store(false);
    LOG("[WorkshopProvider] Initialized: workspace=%s appid=%u visibility=%d tool=%s",
        m_root.c_str(), m_workshopAppid, m_visibility, m_toolPath.c_str());

    m_pushThread = std::thread([this] { PushLoop(); });

    // Prefetch: pull every (accountId, appId) pair this workspace has seen
    // before, so cross-device restores are usually complete before a game
    // first asks for its cloud files.
    {
        std::vector<std::pair<uint32_t, uint32_t>> pairs;
        std::error_code mec;
        for (auto& entry : std::filesystem::directory_iterator(
                 FileUtil::LongPath(FileUtil::Utf8ToPath(MetaDir())), mec)) {
            std::string name = FileUtil::PathToUtf8(entry.path().filename());
            if (name.size() < 7 || name.rfind(".json") != name.size() - 5) continue;
            size_t sep = name.find('_');
            if (sep == std::string::npos) continue;
            uint32_t a = 0, b = 0;
            try {
                a = (uint32_t)std::stoul(name.substr(0, sep));
                b = (uint32_t)std::stoul(name.substr(sep + 1, name.size() - 5 - sep - 1));
            } catch (...) { continue; }
            if (a && b) pairs.emplace_back(a, b);
        }
        if (!pairs.empty()) {
            m_prefetchThread = std::thread([this, pairs] {
                for (auto& [a, b] : pairs) {
                    if (m_shutdown.load()) return;
                    EnsurePulled(a, b);
                }
            });
        }
    }
    return true;
#endif
}

void WorkshopProvider::Shutdown() {
    if (!m_initialized) return;
    m_initialized = false;
    m_shutdown.store(true);
    m_pushCv.notify_all();

#ifdef _WIN32
    // A worker process can block for minutes; kill it so Shutdown (and Steam
    // exit) is not held hostage. RunTool clears the handle itself, so only
    // terminate -- never close -- here.
    {
        std::lock_guard<std::mutex> lock(m_activeProcessMtx);
        if (m_activeToolProcess) {
            TerminateProcess(m_activeToolProcess, 1);
        }
    }
#endif

    if (m_pushThread.joinable()) m_pushThread.join();
    if (m_prefetchThread.joinable()) m_prefetchThread.join();
    LOG("[WorkshopProvider] Shutdown complete");
}

// ── Mirror filesystem helpers ──────────────────────────────────────────

std::string WorkshopProvider::ToFullPath(const std::string& relPath) const {
    std::string full = m_root + relPath;
    for (auto& c : full) {
        if (c == '/') c = '\\';
    }
    if (!FileUtil::IsPathWithin(m_root, full)) {
        LOG("[WorkshopProvider] BLOCKED path traversal: %s (root=%s)",
            relPath.c_str(), m_root.c_str());
        return {};
    }
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(FileUtil::Utf8ToPath(full), ec);
    if (ec) return {};
    if (!FileUtil::IsPathWithin(m_root, FileUtil::PathToUtf8(canonical))) {
        LOG("[WorkshopProvider] BLOCKED path traversal after canonicalization: %s (root=%s)",
            relPath.c_str(), m_root.c_str());
        return {};
    }
    return FileUtil::PathToUtf8(canonical);
}

std::string WorkshopProvider::ItemRootDir(uint32_t accountId, uint32_t appId) const {
    // No trailing separator: workshop_sync_tool rejects it, and a trailing
    // backslash would break command-line quoting downstream.
    return m_root + std::to_string(accountId) + "\\" + std::to_string(appId);
}

std::string WorkshopProvider::MetaDir() const {
    return m_root + "meta\\";
}

// ── Per-item state ─────────────────────────────────────────────────────

bool WorkshopProvider::ParseItemPath(const std::string& path, uint32_t& accountId,
                                     uint32_t& appId) {
    size_t slash1 = path.find('/');
    if (slash1 == std::string::npos) return false;
    size_t slash2 = path.find('/', slash1 + 1);
    if (slash2 == std::string::npos || slash2 == slash1 + 1) return false;
    try {
        size_t consumed = 0;
        accountId = (uint32_t)std::stoul(path.substr(0, slash1), &consumed);
        if (consumed != slash1) return false;
        consumed = 0;
        appId = (uint32_t)std::stoul(path.substr(slash1 + 1, slash2 - slash1 - 1), &consumed);
        if (consumed != slash2 - slash1 - 1) return false;
    } catch (...) {
        return false;
    }
    // appId may be 0: account-scope blobs ({accountId}/0/stats.json) live in a
    // per-account Workshop item keyed with game appid 0.
    return accountId != 0;
}

std::shared_ptr<WorkshopProvider::ItemState> WorkshopProvider::GetItemState(
    uint32_t accountId, uint32_t appId) {
    uint64_t key = ((uint64_t)accountId << 32) | appId;
    std::lock_guard<std::mutex> lock(m_registryMtx);
    auto it = m_items.find(key);
    if (it == m_items.end())
        it = m_items.emplace(key, std::make_shared<ItemState>()).first;
    return it->second;
}

void WorkshopProvider::LoadMeta(const std::shared_ptr<ItemState>& st,
                                uint32_t accountId, uint32_t appId) {
    std::string path = MetaDir() + std::to_string(accountId) + "_" +
        std::to_string(appId) + ".json";
    std::ifstream f(FileUtil::Utf8ToPath(path));
    if (f) {
        std::string text((std::istreambuf_iterator<char>(f)), {});
        f.close();
        auto root = Json::Parse(text);
        if (root["item"].type == Json::Type::Number)
            st->itemId = (uint64_t)root["item"].integer();
        if (root["tombstones"].type == Json::Type::Object) {
            for (auto& [rel, ts] : root["tombstones"].objVal) {
                if (ts.type == Json::Type::Number)
                    st->tombstones[rel] = (uint64_t)ts.integer();
            }
        }
    }

    // The .dirty marker survives a crashed session: restore the flag so the
    // next EnsurePulled pushes local changes before merging remote content.
    // Checked even when no meta json exists yet (a first push never
    // completing leaves a dirty marker but no json).
    std::string dirtyPath = MetaDir() + std::to_string(accountId) + "_" +
        std::to_string(appId) + ".dirty";
    std::error_code ec;
    if (std::filesystem::exists(FileUtil::Utf8ToPath(dirtyPath), ec)) {
        st->dirty = true;
        st->dirtySince = std::chrono::steady_clock::now();
    }
}

void WorkshopProvider::SaveMeta(const std::shared_ptr<ItemState>& st,
                                uint32_t accountId, uint32_t appId) {
    std::string path = MetaDir() + std::to_string(accountId) + "_" +
        std::to_string(appId) + ".json";
    Json::Value root = Json::Object();
    root.objVal["account"] = Json::Number(accountId);
    root.objVal["app"] = Json::Number(appId);
    root.objVal["item"] = Json::Number((double)st->itemId);
    Json::Value tombstones = Json::Object();
    for (auto& [rel, ts] : st->tombstones)
        tombstones.objVal[rel] = Json::Number((double)ts);
    root.objVal["tombstones"] = tombstones;
    if (!FileUtil::AtomicWriteText(path, Json::Stringify(root))) {
        LOG("[WorkshopProvider] Failed to write meta %s", path.c_str());
    }
}

void WorkshopProvider::WriteDirtyFlag(uint32_t accountId, uint32_t appId) {
    std::string path = MetaDir() + std::to_string(accountId) + "_" +
        std::to_string(appId) + ".dirty";
    FileUtil::AtomicWriteText(path, "1");
}

void WorkshopProvider::ClearDirtyFlag(uint32_t accountId, uint32_t appId) {
    std::string path = MetaDir() + std::to_string(accountId) + "_" +
        std::to_string(appId) + ".dirty";
    std::error_code ec;
    std::filesystem::remove(FileUtil::Utf8ToPath(path), ec);
}

void WorkshopProvider::Notify(const std::string& message) {
    LOG("[WorkshopProvider] %s", message.c_str());
    if (m_notifyFn) m_notifyFn(message);
}

// ── Worker tool interaction ────────────────────────────────────────────

#ifdef _WIN32
// Correct CommandLineToArgvW-compatible quoting: double runs of backslashes
// before quotes and the trailing backslash run (a lone trailing backslash
// would otherwise escape the closing quote and corrupt every following arg).
static std::string QuoteArg(const std::string& a) {
    if (a.empty()) return "\"\"";
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : a) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
            backslashes = 0;
        } else {
            out.append(backslashes, '\\');
            out += c;
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

int WorkshopProvider::RunTool(const std::vector<std::string>& args,
                              std::string& stdoutOut, int timeoutSec) {
    std::lock_guard<std::mutex> lock(m_toolMutex);
    if (m_shutdown.load()) return -1;

    std::string cmdline = QuoteArg(m_toolPath);
    for (const auto& a : args) {
        cmdline += ' ';
        cmdline += QuoteArg(a);
    }

    int wideLen = MultiByteToWideChar(CP_UTF8, 0, cmdline.c_str(), (int)cmdline.size(),
                                      nullptr, 0);
    std::wstring wcmd((size_t)(wideLen > 0 ? wideLen : 0), L'\0');
    if (wideLen > 0)
        MultiByteToWideChar(CP_UTF8, 0, cmdline.c_str(), (int)cmdline.size(),
                            wcmd.data(), wideLen);

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
        LOG("[WorkshopProvider] CreatePipe failed (err %lu)", GetLastError());
        return -1;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        DWORD err = GetLastError();
        LOG("[WorkshopProvider] Failed to spawn %s (err %lu)",
            m_toolPath.c_str(), err);
        if (err == ERROR_FILE_NOT_FOUND && !m_notifiedMissingTool.exchange(true)) {
            Notify(std::string("workshop_sync_tool.exe is missing at '") + m_toolPath +
                   "'. Workshop sync is disabled. Run the CloudRedirect app once "
                   "to reinstall it (or copy it there manually), then restart Steam.");
        }
        CloseHandle(outRead);
        CloseHandle(outWrite);
        return -1;
    }
    CloseHandle(outWrite);
    {
        std::lock_guard<std::mutex> lock(m_activeProcessMtx);
        m_activeToolProcess = pi.hProcess;
    }

    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeoutSec * 1000;
    char buf[4096];
    DWORD read = 0;
    bool timedOut = false;
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) break;
        DWORD avail = 0;
        if (PeekNamedPipe(outRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            if (ReadFile(outRead, buf, sizeof(buf) - 1, &read, nullptr) && read > 0) {
                buf[read] = '\0';
                stdoutOut.append(buf, read);
            }
        }
        if (GetTickCount64() >= deadline) {
            LOG("[WorkshopProvider] Tool %s timed out after %ds; killing",
                m_toolPath.c_str(), timeoutSec);
            TerminateProcess(pi.hProcess, 1);
            timedOut = true;
            break;
        }
    }
    // Drain remaining output.
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(outRead, nullptr, 0, nullptr, &avail, nullptr) || avail == 0)
            break;
        if (!ReadFile(outRead, buf, sizeof(buf) - 1, &read, nullptr) || read == 0)
            break;
        buf[read] = '\0';
        stdoutOut.append(buf, read);
    }
    int rc = -1;
    if (!timedOut) {
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        rc = (int)exitCode;
    }
    {
        std::lock_guard<std::mutex> lock(m_activeProcessMtx);
        m_activeToolProcess = nullptr;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(outRead);
    return rc;
}
#else
int WorkshopProvider::RunTool(const std::vector<std::string>&, std::string&, int) {
    return -1;
}
#endif

int WorkshopProvider::PushItem(uint32_t accountId, uint32_t appId,
                               const std::shared_ptr<ItemState>& st) {
    {
        std::lock_guard<std::mutex> lock(st->mtx);
        if (st->pushInFlight) return -1;
        st->pushInFlight = true;
    }

    // Multi-device safety: merge the item's current content before replacing
    // it. A push from a stale mirror would clobber saves uploaded from
    // another machine after our last pull. If the fresh pull fails, skip this
    // push (dirty stays set, PushLoop backs off and retries).
    {
        bool needPull = false;
        std::unique_lock<std::mutex> lk(st->mtx);
        while (st->pullInFlight && !m_shutdown.load())
            st->cv.wait_for(lk, std::chrono::milliseconds(100));
        auto now = std::chrono::steady_clock::now();
        needPull = !(st->pullDone && now < st->pullFreshUntil);
        if (needPull)
            st->pullInFlight = true;
        lk.unlock();

        if (needPull) {
            int prc = PullItem(accountId, appId, st);
            lk.lock();
            st->pullInFlight = false;
            if (prc == 0) {
                st->pullDone = true;
                st->pullFreshUntil = std::chrono::steady_clock::now() +
                    std::chrono::seconds(m_pullFreshnessSec);
            }
            st->cv.notify_all();
            lk.unlock();
            if (prc != 0) {
                LOG("[WorkshopProvider] Push %u/%u skipped: pre-push pull failed (rc=%d); will retry",
                    accountId, appId, prc);
                std::lock_guard<std::mutex> lock(st->mtx);
                st->pushInFlight = false;
                return prc;
            }
        }
    }

    std::string content = ItemRootDir(accountId, appId);
    std::string out;
    int rc = RunTool(
        {"push", "--content", content,
         "--game-appid", std::to_string(appId),
         "--workshop-appid", std::to_string(m_workshopAppid),
         "--visibility", std::to_string(m_visibility),
         "--account", std::to_string(accountId)},
        out, kPushWaitSec);

    std::lock_guard<std::mutex> lock(st->mtx);
    st->pushInFlight = false;

    if (rc == 0) {
        // Extract the item id from "ITEM <id>".
        size_t pos = out.find("ITEM ");
        if (pos != std::string::npos) {
            try {
                st->itemId = std::stoull(out.substr(pos + 5));
            } catch (...) {}
        }
        st->dirty = false;
        st->tombstones.clear();
        ClearDirtyFlag(accountId, appId);
        SaveMeta(st, accountId, appId);
        LOG("[WorkshopProvider] Pushed %u/%u to item %llu",
            accountId, appId, (unsigned long long)st->itemId);
    } else if (rc == kToolAccountMismatch) {
        // Logged-in account changed; do not sync this item under the wrong
        // account. Keep dirty so it is retried after the account switch back.
        LOG("[WorkshopProvider] Push %u/%u skipped: account mismatch", accountId, appId);
    } else {
        LOG("[WorkshopProvider] Push %u/%u failed (rc=%d): %s",
            accountId, appId, rc, out.c_str());
    }
    return rc;
}

int WorkshopProvider::PullItem(uint32_t accountId, uint32_t appId,
                               const std::shared_ptr<ItemState>& st) {
    std::string content = ItemRootDir(accountId, appId);
    std::string out;
    int rc = RunTool(
        {"pull", "--content", content,
         "--game-appid", std::to_string(appId),
         "--workshop-appid", std::to_string(m_workshopAppid),
         "--account", std::to_string(accountId),
         "--timeout-sec", std::to_string(m_pullTimeoutSec)},
        out, m_pullTimeoutSec + kPullWaitMarginSec);

    if (rc == 0) {
        size_t pos = out.find("ITEM ");
        if (pos != std::string::npos) {
            try {
                st->itemId = std::stoull(out.substr(pos + 5));
            } catch (...) {}
        }
        // The merge in the tool copies newer remote files in; re-apply local
        // tombstones so files deleted locally are not resurrected.
        for (auto& [rel, ts] : st->tombstones) {
            std::string full = ToFullPath(
                std::to_string(accountId) + "/" + std::to_string(appId) + "/" + rel);
            if (full.empty()) continue;
            std::error_code ec;
            std::filesystem::remove(FileUtil::LongPath(FileUtil::Utf8ToPath(full)), ec);
        }
        SaveMeta(st, accountId, appId);
        LOG("[WorkshopProvider] Pulled %u/%u from item %llu%s",
            accountId, appId, (unsigned long long)st->itemId,
            pos == std::string::npos ? " (no item published yet)" : "");
    } else {
        LOG("[WorkshopProvider] Pull %u/%u failed (rc=%d): %s",
            accountId, appId, rc, out.c_str());
    }
    return rc;
}

void WorkshopProvider::EnsurePulled(uint32_t accountId, uint32_t appId) {
    if (m_shutdown.load()) return;
    auto st = GetItemState(accountId, appId);

    std::unique_lock<std::mutex> lk(st->mtx);
    auto now = std::chrono::steady_clock::now();
    if (st->pullDone && now < st->pullFreshUntil) return; // mirror still fresh
    if (st->pullInFlight) {
        st->cv.wait_for(lk, std::chrono::seconds(m_pullTimeoutSec + kPullWaitMarginSec),
                        [&] { return st->pullDone || m_shutdown.load(); });
        return;
    }
    if (st->pullAttempts > 0 &&
        now - st->lastPullAttempt < std::chrono::seconds(kPullRetryBackoffSec)) {
        // Recently failed: back off and serve the mirror as-is this call.
        return;
    }
    if (st->pullAttempts >= kMaxPullAttempts) {
        if (st->pullAttempts == kMaxPullAttempts) {
            ++st->pullAttempts; // log once
            LOG("[WorkshopProvider] Giving up on pull %u/%u this session after %d attempts",
                accountId, appId, kMaxPullAttempts);
        }
        return;
    }
    st->pullInFlight = true;
    ++st->pullAttempts;
    lk.unlock();

    LoadMeta(st, accountId, appId);

    // Merge remote content FIRST. The merge keeps newer local files and
    // respects local tombstones, so unsynced local changes survive; the dirty
    // push (PushLoop) then uploads the merged tree. Pushing stale local
    // content before the merge would overwrite another machine's newer saves.
    int rc = PullItem(accountId, appId, st);
    if (rc != 0 && st->pullAttempts == 1) {
        // First pull attempt of the session: Steam may still be starting when
        // a game launches early. One inline retry covers that common case.
        LOG("[WorkshopProvider] First pull %u/%u failed (rc=%d); retrying in %ds",
            accountId, appId, rc, kPullInlineRetryDelaySec);
        std::this_thread::sleep_for(std::chrono::seconds(kPullInlineRetryDelaySec));
        rc = PullItem(accountId, appId, st);
    }

    lk.lock();
    st->pullInFlight = false;
    st->lastPullAttempt = std::chrono::steady_clock::now();
    if (rc == 0) {
        st->pullDone = true;
        st->pullFreshUntil = std::chrono::steady_clock::now() +
            std::chrono::seconds(m_pullFreshnessSec);
    } else {
        LOG("[WorkshopProvider] Pull %u/%u failed (rc=%d); will retry (attempt %d/%d)",
            accountId, appId, rc, st->pullAttempts, kMaxPullAttempts);
    }
    st->cv.notify_all();
}

void WorkshopProvider::MarkDirty(uint32_t accountId, uint32_t appId) {
    auto st = GetItemState(accountId, appId);
    {
        std::lock_guard<std::mutex> lock(st->mtx);
        st->dirty = true;
        st->dirtySince = std::chrono::steady_clock::now();
    }
    WriteDirtyFlag(accountId, appId);
    m_pushCv.notify_one();
}

void WorkshopProvider::PushLoop() {
    while (!m_shutdown.load()) {
        std::shared_ptr<ItemState> candidate;
        uint32_t accountId = 0, appId = 0;
        {
            std::lock_guard<std::mutex> lock(m_registryMtx);
            auto now = std::chrono::steady_clock::now();
            for (auto& [key, st] : m_items) {
                std::lock_guard<std::mutex> slk(st->mtx);
                if (!st->dirty || st->pushInFlight) continue;
                if (now < st->dirtySince + std::chrono::seconds(m_pushDelaySec)) continue;
                if (now < st->pushRetryAfter) continue;
                candidate = st;
                accountId = (uint32_t)(key >> 32);
                appId = (uint32_t)(key & 0xFFFFFFFFu);
                break;
            }
        }

        if (!candidate) {
            std::unique_lock<std::mutex> lk(m_pushMtx);
            m_pushCv.wait_for(lk, std::chrono::seconds(2),
                              [&] { return m_shutdown.load(); });
            continue;
        }

        int rc = PushItem(accountId, appId, candidate);
        if (rc == kToolAgreement) {
            Notify("Steam Workshop: the Workshop legal agreement must be accepted "
                   "before CloudRedirect can upload your saves. Accept it in Steam "
                   "(https://steamcommunity.com/sharedfiles/workshopagreement/) -- "
                   "CloudRedirect will retry automatically.");
            std::lock_guard<std::mutex> lock(candidate->mtx);
            candidate->pushRetryAfter = std::chrono::steady_clock::now() +
                std::chrono::seconds(kRetryAfterAgreementSec);
        } else if (rc == kToolRateLimit) {
            std::lock_guard<std::mutex> lock(candidate->mtx);
            candidate->pushRetryAfter = std::chrono::steady_clock::now() +
                std::chrono::seconds(kRetryAfterRateLimitSec);
        } else if (rc != 0) {
            std::lock_guard<std::mutex> lock(candidate->mtx);
            candidate->pushRetryAfter = std::chrono::steady_clock::now() +
                std::chrono::seconds(kRetryAfterOtherSec);
        }
    }
}

// ── ICloudProvider interface ───────────────────────────────────────────

bool WorkshopProvider::Upload(const std::string& path,
                              const uint8_t* data, size_t len) {
    uint32_t accountId = 0, appId = 0;
    if (!ParseItemPath(path, accountId, appId)) return false;
    EnsurePulled(accountId, appId);

    std::string full = ToFullPath(path);
    if (full.empty()) return false;
    auto fullPath = FileUtil::LongPath(FileUtil::Utf8ToPath(full));
    std::error_code ec;
    std::filesystem::create_directories(fullPath.parent_path(), ec);
    if (ec) {
        LOG("[WorkshopProvider] Upload: failed to create dirs for %s: %s",
            full.c_str(), ec.message().c_str());
        return false;
    }
    if (!FileUtil::AtomicWriteBinary(full, data, len)) {
        LOG("[WorkshopProvider] Upload: atomic write failed %s (%zu bytes)",
            full.c_str(), len);
        return false;
    }

    // A re-uploaded path stops being a tombstone.
    size_t prefix = std::to_string(accountId).size() + 1 + std::to_string(appId).size() + 1;
    if (path.size() > prefix) {
        auto st = GetItemState(accountId, appId);
        std::lock_guard<std::mutex> lock(st->mtx);
        st->tombstones.erase(path.substr(prefix));
    }
    MarkDirty(accountId, appId);
    return true;
}

bool WorkshopProvider::Download(const std::string& path,
                                std::vector<uint8_t>& outData) {
    uint32_t accountId = 0, appId = 0;
    if (!ParseItemPath(path, accountId, appId)) return false;
    EnsurePulled(accountId, appId);

    std::string full = ToFullPath(path);
    if (full.empty()) return false;
    std::ifstream f(FileUtil::LongPath(FileUtil::Utf8ToPath(full)), std::ios::binary);
    if (!f) return false;
    outData.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool WorkshopProvider::Remove(const std::string& path) {
    uint32_t accountId = 0, appId = 0;
    if (!ParseItemPath(path, accountId, appId)) return false;
    EnsurePulled(accountId, appId);

    std::string full = ToFullPath(path);
    if (full.empty()) return false;
    std::error_code ec;
    std::filesystem::remove(FileUtil::LongPath(FileUtil::Utf8ToPath(full)), ec);

    // Tombstone the deletion so a later pull merge cannot resurrect it.
    size_t prefix = std::to_string(accountId).size() + 1 + std::to_string(appId).size() + 1;
    if (path.size() > prefix) {
        auto st = GetItemState(accountId, appId);
        std::lock_guard<std::mutex> lock(st->mtx);
        st->tombstones[path.substr(prefix)] = (uint64_t)std::chrono::duration_cast<
            std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        SaveMeta(st, accountId, appId);
    }
    MarkDirty(accountId, appId);
    // Success if removed or never existed.
    return !ec;
}

ICloudProvider::ExistsStatus WorkshopProvider::CheckExists(const std::string& path) {
    uint32_t accountId = 0, appId = 0;
    if (!ParseItemPath(path, accountId, appId)) return ExistsStatus::Error;
    EnsurePulled(accountId, appId);

    std::string full = ToFullPath(path);
    if (full.empty()) return ExistsStatus::Error;
    std::error_code ec;
    auto fsPath = FileUtil::LongPath(FileUtil::Utf8ToPath(full));
    bool exists = std::filesystem::exists(fsPath, ec);
    if (ec) return ExistsStatus::Error;
    if (!exists) return ExistsStatus::Missing;
    bool regular = std::filesystem::is_regular_file(fsPath, ec);
    if (ec) return ExistsStatus::Error;
    return regular ? ExistsStatus::Exists : ExistsStatus::Missing;
}

std::vector<ICloudProvider::FileInfo> WorkshopProvider::List(const std::string& prefix) {
    std::vector<FileInfo> result;
    ListChecked(prefix, result);
    return result;
}

bool WorkshopProvider::ListChecked(const std::string& prefix,
                                   std::vector<FileInfo>& result,
                                   bool* outComplete) {
    result.clear();
    if (outComplete) *outComplete = false;

    uint32_t accountId = 0, appId = 0;
    if (ParseItemPath(prefix, accountId, appId))
        EnsurePulled(accountId, appId);

    std::string dir = ToFullPath(prefix);
    if (dir.empty()) return false;
    std::error_code ec;
    auto dirPath = FileUtil::LongPath(FileUtil::Utf8ToPath(dir));
    bool exists = std::filesystem::exists(dirPath, ec);
    if (ec) return false;
    if (!exists) { if (outComplete) *outComplete = true; return true; }
    bool isDir = std::filesystem::is_directory(dirPath, ec);
    if (ec) return false;
    if (!isDir) { if (outComplete) *outComplete = true; return true; }

    auto fileClockNow = std::filesystem::file_time_type::clock::now();
    auto sysClockNow = std::chrono::system_clock::now();

    std::error_code rootEc;
    auto rootCanonical = std::filesystem::weakly_canonical(
        FileUtil::Utf8ToPath(m_root), rootEc);
    std::string rootPrefix = FileUtil::MakePathPrefix(rootEc
        ? FileUtil::PathToUtf8(FileUtil::LongPath(FileUtil::Utf8ToPath(m_root)))
        : FileUtil::PathToUtf8(FileUtil::LongPath(rootCanonical)));

    bool sawSkippedEntries = false;
    std::filesystem::recursive_directory_iterator it(dirPath, ec);
    std::filesystem::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto& entry = *it;
        std::error_code ec2;
        bool isFile = entry.is_regular_file(ec2);
        if (ec2) { sawSkippedEntries = true; continue; }
        if (!isFile) continue;

        std::string entryUtf8 = FileUtil::PathToUtf8(entry.path());
        FileUtil::NormalizeSlashesInPlace(entryUtf8);
        std::string rel;
        if (!FileUtil::RelativeUtf8Path(entryUtf8, rootPrefix, &rel)) {
            sawSkippedEntries = true; continue;
        }

        FileInfo fi;
        fi.path = rel;
        fi.size = entry.file_size(ec2);
        if (ec2) { sawSkippedEntries = true; continue; }

        auto ftime = std::filesystem::last_write_time(entry.path(), ec2);
        if (ec2) { sawSkippedEntries = true; continue; }
        auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(
            ftime - fileClockNow + sysClockNow);
        fi.modifiedTime = (uint64_t)sctp.time_since_epoch().count();

        result.push_back(std::move(fi));
    }
    bool ok = !ec;
    if (ok && outComplete) *outComplete = !sawSkippedEntries;
    return ok;
}
