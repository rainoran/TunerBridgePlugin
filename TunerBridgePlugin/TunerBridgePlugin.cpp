// TVTestBridgePlugin.cpp
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shlwapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define TVTEST_PLUGIN_CLASS_IMPLEMENT
#include "TVTestPlugin.h"

#pragma comment(lib, "shlwapi.lib")

namespace
{
    constexpr wchar_t kPluginName[] = L"TVTest Bridge Plugin";
    constexpr wchar_t kPluginDescription[] = L"External bridge for TS forwarding and TVTest control";

    constexpr UINT WM_APP_BRIDGE_EXEC = WM_APP + 0x540;
    constexpr DWORD kDefaultBufferPackets = 20000UL;
    constexpr DWORD kMinBufferPackets = 10000UL;
    constexpr DWORD kMaxBufferPackets = 100000UL;
    constexpr DWORD kDefaultSendPackets = 256UL;
    constexpr DWORD kCommandWaitMs = 5000;
    constexpr DWORD kPipeReconnectSleepMs = 500;
    constexpr uint32_t kTsFrameMagic = 0x31534254U; // 'TSB1'

    struct TsFrameHeader
    {
        uint32_t magic = 0;
        uint32_t payloadBytes = 0;
        uint32_t flags = 0;
        uint32_t reserved = 0;
    };

    struct PluginSettings
    {
        bool bridgeEnabled = true;
        bool verboseLog = true;
        bool autoEnableTs = false;
        bool sendStatusOnConnect = true;

        // 新方式を既定にする。
        // INI が読めなくても TVTestBridge_{TUNER} を使う。
        bool useDidSuffix = false;

        bool forceSetServiceFallback = true;
        DWORD bufferPackets = kDefaultBufferPackets;
        DWORD sendPackets = kDefaultSendPackets;

        // 正式仕様は {TUNER}。
        // 互換として TVTestBridge_X も ResolvePipeTemplate_() 側で読む。
        std::wstring controlPipeName = L"\\\\.\\pipe\\TVTestBridge_{TUNER}";
        std::wstring tsPipeName = L"\\\\.\\pipe\\TVTestBridgeTS_{TUNER}";
    };

    struct StateSnapshot
    {
        bool bridgeEnabled = true;
        int deviceId = -1;
        std::wstring driverName;
        int space = -1;
        int channelIndex = -1;
        WORD serviceId = 0;
        WORD transportStreamId = 0;
        WORD networkId = 0;
        DWORD recordStatus = 0;
        float signalLevel = 0.0f;
        DWORD bitRate = 0;
        DWORD errorPackets = 0;
        DWORD scramblePackets = 0;
        DWORD dropPackets = 0;
        WORD eventIdNow = 0;
        WORD eventIdNext = 0;
        std::wstring eventNameNow;
        std::wstring eventNameNext;
    };

    static std::string WideToUtf8(const std::wstring& src)
    {
        if (src.empty()) {
            return {};
        }

        const int needed = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            src.c_str(),
            static_cast<int>(src.size()),
            nullptr,
            0,
            nullptr,
            nullptr);

        if (needed <= 0) {
            return {};
        }

        std::string out(static_cast<size_t>(needed), '\0');

        ::WideCharToMultiByte(
            CP_UTF8,
            0,
            src.c_str(),
            static_cast<int>(src.size()),
            out.data(),
            needed,
            nullptr,
            nullptr);

        return out;
    }

    static std::wstring Utf8ToWide(const std::string& src)
    {
        if (src.empty()) {
            return {};
        }

        const int needed = ::MultiByteToWideChar(
            CP_UTF8,
            0,
            src.c_str(),
            static_cast<int>(src.size()),
            nullptr,
            0);

        if (needed <= 0) {
            return {};
        }

        std::wstring out(static_cast<size_t>(needed), L'\0');

        ::MultiByteToWideChar(
            CP_UTF8,
            0,
            src.c_str(),
            static_cast<int>(src.size()),
            out.data(),
            needed);

        return out;
    }

    static std::wstring Trim(const std::wstring& s)
    {
        size_t first = 0;
        while (first < s.size() && std::iswspace(s[first])) {
            ++first;
        }

        size_t last = s.size();
        while (last > first && std::iswspace(s[last - 1])) {
            --last;
        }

        return s.substr(first, last - first);
    }

    static std::wstring ToUpperCopy(std::wstring s)
    {
        for (auto& ch : s) {
            ch = static_cast<wchar_t>(std::towupper(ch));
        }
        return s;
    }

    static bool ParseBool(const std::wstring& s, bool defaultValue)
    {
        if (s.empty()) {
            return defaultValue;
        }

        const std::wstring v = ToUpperCopy(Trim(s));

        if (v == L"1" || v == L"TRUE" || v == L"YES" || v == L"ON") {
            return true;
        }

        if (v == L"0" || v == L"FALSE" || v == L"NO" || v == L"OFF") {
            return false;
        }

        return defaultValue;
    }

    static DWORD ClampDword(DWORD v, DWORD minv, DWORD maxv)
    {
        return std::min<DWORD>(std::max<DWORD>(v, minv), maxv);
    }

    static std::vector<std::string> SplitArgs(const std::string& line)
    {
        std::vector<std::string> out;
        std::string cur;
        bool inQuote = false;

        for (size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];

            if (c == '"') {
                inQuote = !inQuote;
                continue;
            }

            if (!inQuote && std::isspace(static_cast<unsigned char>(c))) {
                if (!cur.empty()) {
                    out.push_back(cur);
                    cur.clear();
                }
                continue;
            }

            cur.push_back(c);
        }

        if (!cur.empty()) {
            out.push_back(cur);
        }

        return out;
    }

    static std::wstring JoinWide(const std::vector<std::string>& parts, size_t startIndex)
    {
        std::string tmp;

        for (size_t i = startIndex; i < parts.size(); ++i) {
            if (i > startIndex) {
                tmp.push_back(' ');
            }
            tmp += parts[i];
        }

        return Utf8ToWide(tmp);
    }

    class CCriticalLock
    {
    public:
        CCriticalLock()
        {
            ::InitializeCriticalSection(&m_cs);
        }

        ~CCriticalLock()
        {
            ::DeleteCriticalSection(&m_cs);
        }

        void Lock()
        {
            ::EnterCriticalSection(&m_cs);
        }

        void Unlock()
        {
            ::LeaveCriticalSection(&m_cs);
        }

    private:
        CRITICAL_SECTION m_cs{};
    };

    class AutoLock
    {
    public:
        explicit AutoLock(CCriticalLock& lock)
            : m_lock(lock)
        {
            m_lock.Lock();
        }

        ~AutoLock()
        {
            m_lock.Unlock();
        }

    private:
        CCriticalLock& m_lock;
    };

    struct BridgeCommand
    {
        std::string line;
        HANDLE doneEvent = nullptr;
        std::string response;
    };

    class CTvTestBridgePlugin final : public TVTest::CTVTestPlugin
    {
    public:
        CTvTestBridgePlugin() = default;
        ~CTvTestBridgePlugin() override = default;

        bool GetPluginInfo(TVTest::PluginInfo* pInfo) override
        {
            pInfo->Type = TVTest::PLUGIN_TYPE_NORMAL;
            pInfo->Flags = 0;
            pInfo->pszPluginName = kPluginName;
            pInfo->pszCopyright = L"OpenAI";
            pInfo->pszDescription = kPluginDescription;
            return true;
        }

        bool Initialize() override
        {
            m_modulePath = GetModuleFilePath();
            m_iniPath = BuildIniPath(m_modulePath);

            // INI が無い場合だけ自動生成する。
            // 既存 INI は上書きしない。
            EnsureDefaultIniFile_();

            m_deviceId = GetDeviceID(::GetCommandLineW(), m_driverArgName);

            LoadSettings();
            CreateHiddenWindow();
            AllocateTsBuffer();

            if (m_settings.verboseLog) {
                Log(L"TVTest Bridge Plugin を初期化します。");
                Log(std::wstring(L"設定ファイル: ") + m_iniPath);
            }

            m_running = true;
            m_tsThread = std::thread(&CTvTestBridgePlugin::TsSenderThread, this);
            m_controlThread = std::thread(&CTvTestBridgePlugin::ControlPipeThread, this);
            m_tsPipeThread = std::thread(&CTvTestBridgePlugin::TsPipeThread, this);

            m_pApp->SetEventCallback(EventCallback, this);

            if (m_settings.bridgeEnabled) {
                m_pApp->SetStreamCallback(0, StreamCallback, this);
                SnapshotAndNotify(L"INIT");
            }
            else {
                Log(L"TVTest 連携は設定で無効です。");
            }

            return true;
        }

        bool Finalize() override
        {
            if (m_settings.verboseLog) {
                Log(L"TVTest Bridge Plugin の終了処理を開始します。");
            }

            if (m_pApp) {
                m_pApp->SetStreamCallback(TVTest::STREAM_CALLBACK_REMOVE, StreamCallback);
                m_pApp->SetEventCallback(nullptr, nullptr);
            }

            m_running = false;

            if (m_hTsWakeEvent) {
                ::SetEvent(m_hTsWakeEvent);
            }

            DisconnectPipeHandle(m_controlPipeClient);
            DisconnectPipeHandle(m_tsPipeClient);

            if (m_controlPipeListener != INVALID_HANDLE_VALUE) {
                ::CancelIoEx(m_controlPipeListener, nullptr);
                ::CloseHandle(m_controlPipeListener);
                m_controlPipeListener = INVALID_HANDLE_VALUE;
            }

            if (m_tsPipeListener != INVALID_HANDLE_VALUE) {
                ::CancelIoEx(m_tsPipeListener, nullptr);
                ::CloseHandle(m_tsPipeListener);
                m_tsPipeListener = INVALID_HANDLE_VALUE;
            }

            JoinThread(m_controlThread);
            JoinThread(m_tsPipeThread);
            JoinThread(m_tsThread);

            if (m_hTsWakeEvent) {
                ::CloseHandle(m_hTsWakeEvent);
                m_hTsWakeEvent = nullptr;
            }

            if (m_hiddenWnd) {
                ::DestroyWindow(m_hiddenWnd);
                m_hiddenWnd = nullptr;
            }

            if (!m_windowClassName.empty()) {
                ::UnregisterClassW(m_windowClassName.c_str(), g_hinstDLL);
            }

            m_tsBuffer.clear();
            return true;
        }

    private:
        static LRESULT CALLBACK EventCallback(UINT Event, LPARAM lParam1, LPARAM lParam2, void* pClientData)
        {
            auto* self = static_cast<CTvTestBridgePlugin*>(pClientData);
            if (!self) {
                return 0;
            }

            switch (Event) {
            case TVTest::EVENT_PLUGINENABLE:
                self->m_pluginEnabledByHost = (lParam1 != 0);
                self->NotifyLine(
                    self->BuildSimpleEventLine(
                        L"PLUGINENABLE",
                        self->m_pluginEnabledByHost ? L"1" : L"0"));
                break;

            case TVTest::EVENT_CHANNELCHANGE:
                self->PurgeTsBuffer();
                self->SnapshotAndNotify(L"CHANNELCHANGE");
                break;

            case TVTest::EVENT_SERVICECHANGE:
                self->PurgeTsBuffer();
                self->SnapshotAndNotify(L"SERVICECHANGE");
                break;

            case TVTest::EVENT_DRIVERCHANGE:
                self->PurgeTsBuffer();
                self->SnapshotAndNotify(L"DRIVERCHANGE");
                break;

            case TVTest::EVENT_SERVICEUPDATE:
                self->SnapshotAndNotify(L"SERVICEUPDATE");
                break;

            case TVTest::EVENT_RECORDSTATUSCHANGE:
                self->SnapshotAndNotify(L"RECORDSTATUSCHANGE");
                break;

#if TVTEST_PLUGIN_VERSION >= TVTEST_PLUGIN_VERSION_(0,0,10)
            case TVTest::EVENT_STARTRECORD:
                self->SnapshotAndNotify(L"STARTRECORD");
                break;

            case TVTest::EVENT_RELAYRECORD:
                self->SnapshotAndNotify(L"RELAYRECORD");
                break;
#endif

#if TVTEST_PLUGIN_VERSION >= TVTEST_PLUGIN_VERSION_(0,0,9)
            case TVTest::EVENT_SETTINGSCHANGE:
                self->LoadSettings();
                self->NotifyLine(
                    self->BuildSimpleEventLine(L"SETTINGSCHANGE", L"reloaded"));
                break;
#endif

            default:
                break;
            }

            return 0;
        }

        static BOOL CALLBACK StreamCallback(BYTE* pData, void* pClientData)
        {
            auto* self = static_cast<CTvTestBridgePlugin*>(pClientData);
            if (!self || !self->m_running || !self->m_settings.bridgeEnabled) {
                return TRUE;
            }

            {
                AutoLock lock(self->m_tsLock);

                if (self->m_streamBufferUsed == self->m_streamBufferPackets) {
                    ++self->m_dropCount;
                    return TRUE;
                }

                const size_t writeIndex =
                    (self->m_streamBufferPos + self->m_streamBufferUsed) %
                    self->m_streamBufferPackets;

                ::CopyMemory(
                    self->m_tsBuffer.data() + (writeIndex * 188),
                    pData,
                    188);

                ++self->m_streamBufferUsed;
            }

            if (self->m_tsForwardEnabled && self->m_hTsWakeEvent) {
                ::SetEvent(self->m_hTsWakeEvent);
            }

            return TRUE;
        }

        static LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT message, WPARAM, LPARAM lParam)
        {
            auto* self =
                reinterpret_cast<CTvTestBridgePlugin*>(
                    ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

            if (message == WM_NCCREATE) {
                const auto* pcs =
                    reinterpret_cast<CREATESTRUCTW*>(lParam);

                ::SetWindowLongPtrW(
                    hwnd,
                    GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(pcs->lpCreateParams));

                return TRUE;
            }

            if (!self) {
                return ::DefWindowProcW(hwnd, message, 0, lParam);
            }

            if (message == WM_APP_BRIDGE_EXEC) {
                auto* raw = reinterpret_cast<BridgeCommand*>(lParam);

                if (raw) {
                    raw->response = self->ExecuteCommandOnUiThread(raw->line);

                    if (raw->doneEvent) {
                        ::SetEvent(raw->doneEvent);
                    }
                }

                return 0;
            }

            return ::DefWindowProcW(hwnd, message, 0, lParam);
        }

        void JoinThread(std::thread& th)
        {
            if (th.joinable()) {
                th.join();
            }
        }

        void Log(const std::wstring& text)
        {
            if (m_pApp) {
                m_pApp->AddLog(text.c_str());
            }
        }

        std::wstring GetModuleFilePath() const
        {
            wchar_t path[MAX_PATH]{};

            if (::GetModuleFileNameW(g_hinstDLL, path, MAX_PATH) <= 0) {
                return {};
            }

            return path;
        }

        std::wstring BuildIniPath(const std::wstring& modulePath) const
        {
            if (modulePath.empty()) {
                return {};
            }

            wchar_t buf[MAX_PATH]{};
            const errno_t rc = wcscpy_s(buf, modulePath.c_str());

            if (rc != 0) {
                return {};
            }

            if (!::PathRenameExtensionW(buf, L".ini")) {
                return {};
            }

            return buf;
        }

        static int ParseDeviceIdToken_(const wchar_t* text) noexcept
        {
            if (text == nullptr || text[0] == L'\0') {
                return -1;
            }

            const wchar_t ch = text[0];

            if (ch >= L'A' && ch <= L'Z') {
                return static_cast<int>(ch - L'A');
            }

            if (ch >= L'a' && ch <= L'z') {
                return static_cast<int>(ch - L'a');
            }

            wchar_t* endPtr = nullptr;
            const long value = std::wcstol(text, &endPtr, 10);

            if (endPtr != text && value >= 0 && value <= 25) {
                return static_cast<int>(value);
            }

            return -2;
        }

        static int GetDeviceID(LPCWSTR pszCommandLine, std::wstring& driverName)
        {
            driverName.clear();

            if (pszCommandLine == nullptr || pszCommandLine[0] == L'\0') {
                return -1;
            }

            int argc = 0;
            LPWSTR* argv = ::CommandLineToArgvW(pszCommandLine, &argc);

            if (argv == nullptr) {
                return -1;
            }

            int deviceId = -1;

            for (int i = 0; i < argc - 1; ++i) {
                if (::lstrcmpiW(argv[i], L"/DID") == 0) {
                    deviceId = ParseDeviceIdToken_(argv[i + 1]);
                }
                else if (::lstrcmpiW(argv[i], L"/d") == 0) {
                    driverName = argv[i + 1];
                }
            }

            ::LocalFree(argv);

            if (deviceId < 0) {
                driverName.clear();
            }

            return deviceId;
        }

        std::wstring GetDidSectionName() const
        {
            if (m_deviceId < 0 || m_deviceId > 25) {
                return {};
            }

            wchar_t buf[16]{};
            ::wsprintfW(buf, L"DID_%c", L'A' + m_deviceId);
            return buf;
        }

        static void ReplaceAll_(
            std::wstring& text,
            const std::wstring& from,
            const std::wstring& to)
        {
            if (from.empty()) {
                return;
            }

            std::size_t pos = 0;

            while ((pos = text.find(from, pos)) != std::wstring::npos) {
                text.replace(pos, from.size(), to);
                pos += to.size();
            }
        }

        static bool HasPipePlaceholder_(const std::wstring& text)
        {
            return
                text.find(L"{TUNER}") != std::wstring::npos ||
                text.find(L"%TUNER%") != std::wstring::npos ||
                text.find(L"{X}") != std::wstring::npos ||
                text.find(L"%X%") != std::wstring::npos ||
                text.find(L"_X") != std::wstring::npos ||
                text.find(L"{DID}") != std::wstring::npos ||
                text.find(L"%DID%") != std::wstring::npos;
        }

        static std::wstring ResolvePipeTemplate_(std::wstring text, int deviceId)
        {
            text = Trim(text);

            if (text.empty()) {
                return {};
            }

            int safeId = deviceId;

            if (safeId < 0) {
                safeId = 0;
            }
            if (safeId > 25) {
                safeId = 25;
            }

            const std::wstring number = std::to_wstring(safeId);

            wchar_t letterBuf[8]{};
            ::wsprintfW(letterBuf, L"%c", L'A' + safeId);
            const std::wstring letter = letterBuf;

            ReplaceAll_(text, L"{TUNER}", number);
            ReplaceAll_(text, L"%TUNER%", number);

            ReplaceAll_(text, L"{X}", number);
            ReplaceAll_(text, L"%X%", number);

            ReplaceAll_(text, L"_X", L"_" + number);

            ReplaceAll_(text, L"{DID}", letter);
            ReplaceAll_(text, L"%DID%", letter);

            return text;
        }

        static bool EndsWithNoCase_(
            const std::wstring& text,
            const std::wstring& suffix)
        {
            if (suffix.empty() || text.size() < suffix.size()) {
                return false;
            }

            const std::wstring tail =
                text.substr(text.size() - suffix.size());

            return ::lstrcmpiW(tail.c_str(), suffix.c_str()) == 0;
        }

        static void AppendSuffixIfMissing_(
            std::wstring& text,
            const std::wstring& suffix)
        {
            if (text.empty() || suffix.empty()) {
                return;
            }

            if (!EndsWithNoCase_(text, suffix)) {
                text += suffix;
            }
        }

        std::wstring GetTunerSectionName_() const
        {
            if (m_deviceId < 0 || m_deviceId > 25) {
                return {};
            }

            return L"Tuner" + std::to_wstring(m_deviceId);
        }

        std::wstring GetDidNumberSectionName_() const
        {
            if (m_deviceId < 0 || m_deviceId > 25) {
                return {};
            }

            return L"DID_" + std::to_wstring(m_deviceId);
        }

        std::wstring GetDidLetterSectionName_() const
        {
            if (m_deviceId < 0 || m_deviceId > 25) {
                return {};
            }

            wchar_t buf[16]{};
            ::wsprintfW(buf, L"DID_%c", L'A' + m_deviceId);
            return buf;
        }

        std::wstring ReadPipeValueFromSection_(
            const std::wstring& section,
            const wchar_t* key) const
        {
            if (section.empty() || key == nullptr || key[0] == L'\0') {
                return {};
            }

            return Trim(GetIniString(section.c_str(), key, L""));
        }

        static bool ReadAllBytesFromFileW_(
            const std::wstring& path,
            std::vector<unsigned char>& outBytes,
            DWORD& outError) noexcept
        {
            outBytes.clear();
            outError = ERROR_SUCCESS;

            if (path.empty()) {
                outError = ERROR_PATH_NOT_FOUND;
                return false;
            }

            HANDLE hFile = ::CreateFileW(
                path.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);

            if (hFile == INVALID_HANDLE_VALUE) {
                outError = ::GetLastError();
                return false;
            }

            LARGE_INTEGER size{};

            if (!::GetFileSizeEx(hFile, &size)) {
                outError = ::GetLastError();
                ::CloseHandle(hFile);
                return false;
            }

            if (size.QuadPart <= 0) {
                outError = ERROR_HANDLE_EOF;
                ::CloseHandle(hFile);
                return false;
            }

            if (size.QuadPart > 1024 * 1024) {
                outError = ERROR_INVALID_DATA;
                ::CloseHandle(hFile);
                return false;
            }

            outBytes.resize(static_cast<std::size_t>(size.QuadPart));

            DWORD readBytes = 0;

            const BOOL ok = ::ReadFile(
                hFile,
                outBytes.data(),
                static_cast<DWORD>(outBytes.size()),
                &readBytes,
                nullptr);

            const DWORD err = ok ? ERROR_SUCCESS : ::GetLastError();
            ::CloseHandle(hFile);

            if (!ok) {
                outError = err;
                outBytes.clear();
                return false;
            }

            if (readBytes != outBytes.size()) {
                outBytes.resize(readBytes);
            }

            return !outBytes.empty();
        }

        static std::wstring DecodeBytesToWide_(
            const std::vector<unsigned char>& bytes)
        {
            if (bytes.empty()) {
                return {};
            }

            if (bytes.size() >= 2 &&
                bytes[0] == 0xFF &&
                bytes[1] == 0xFE) {
                const wchar_t* p =
                    reinterpret_cast<const wchar_t*>(bytes.data() + 2);

                const std::size_t count =
                    (bytes.size() - 2) / sizeof(wchar_t);

                return std::wstring(p, p + count);
            }

            std::size_t offset = 0;

            if (bytes.size() >= 3 &&
                bytes[0] == 0xEF &&
                bytes[1] == 0xBB &&
                bytes[2] == 0xBF) {
                offset = 3;
            }

            const char* src =
                reinterpret_cast<const char*>(bytes.data() + offset);

            const int srcSize =
                static_cast<int>(bytes.size() - offset);

            if (srcSize <= 0) {
                return {};
            }

            int needed = ::MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                src,
                srcSize,
                nullptr,
                0);

            if (needed > 0) {
                std::wstring out(static_cast<std::size_t>(needed), L'\0');

                ::MultiByteToWideChar(
                    CP_UTF8,
                    MB_ERR_INVALID_CHARS,
                    src,
                    srcSize,
                    out.data(),
                    needed);

                return out;
            }

            needed = ::MultiByteToWideChar(
                CP_ACP,
                0,
                src,
                srcSize,
                nullptr,
                0);

            if (needed <= 0) {
                return {};
            }

            std::wstring out(static_cast<std::size_t>(needed), L'\0');

            ::MultiByteToWideChar(
                CP_ACP,
                0,
                src,
                srcSize,
                out.data(),
                needed);

            return out;
        }

        static std::wstring StripBomAndTrim_(std::wstring s)
        {
            if (!s.empty() && s.front() == 0xFEFF) {
                s.erase(s.begin());
            }

            return Trim(s);
        }

        static bool SplitIniKeyValue_(
            const std::wstring& line,
            std::wstring& key,
            std::wstring& value)
        {
            const std::size_t pos = line.find(L'=');

            if (pos == std::wstring::npos) {
                return false;
            }

            key = StripBomAndTrim_(line.substr(0, pos));
            value = StripBomAndTrim_(line.substr(pos + 1));

            return !key.empty();
        }

        bool TryReadIniStringManual_(
            const wchar_t* section,
            const wchar_t* key,
            std::wstring& out) const
        {
            out.clear();

            if (m_iniPath.empty() ||
                section == nullptr ||
                key == nullptr ||
                section[0] == L'\0' ||
                key[0] == L'\0') {
                return false;
            }

            std::vector<unsigned char> bytes;
            DWORD readError = ERROR_SUCCESS;

            if (!ReadAllBytesFromFileW_(m_iniPath, bytes, readError)) {
                return false;
            }

            const std::wstring text = DecodeBytesToWide_(bytes);

            if (text.empty()) {
                return false;
            }

            const std::wstring targetSection =
                ToUpperCopy(StripBomAndTrim_(section));

            const std::wstring targetKey =
                ToUpperCopy(StripBomAndTrim_(key));

            std::wistringstream iss(text);
            std::wstring line;
            std::wstring currentSection;

            while (std::getline(iss, line)) {
                line = StripBomAndTrim_(line);

                if (!line.empty() && line.back() == L'\r') {
                    line.pop_back();
                    line = StripBomAndTrim_(line);
                }

                if (line.empty()) {
                    continue;
                }

                if (line[0] == L';' || line[0] == L'#') {
                    continue;
                }

                if (line.front() == L'[') {
                    const std::size_t end = line.find(L']');

                    if (end != std::wstring::npos && end > 1) {
                        currentSection =
                            ToUpperCopy(
                                StripBomAndTrim_(line.substr(1, end - 1)));
                    }

                    continue;
                }

                if (currentSection != targetSection) {
                    continue;
                }

                std::wstring k;
                std::wstring v;

                if (!SplitIniKeyValue_(line, k, v)) {
                    continue;
                }

                if (ToUpperCopy(k) == targetKey) {
                    out = v;
                    return true;
                }
            }

            return false;
        }

        bool WriteDefaultIniFile_() const
        {
            if (m_iniPath.empty()) {
                return false;
            }

            HANDLE hFile = ::CreateFileW(
                m_iniPath.c_str(),
                GENERIC_WRITE,
                0,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);

            if (hFile == INVALID_HANDLE_VALUE) {
                return false;
            }

            std::wstring text;
            text += L"[Settings]\r\n";
            text += L"Enabled=1\r\n";
            text += L"VerboseLog=1\r\n";
            text += L"AutoEnableTs=0\r\n";
            text += L"SendStatusOnConnect=1\r\n";
            text += L"UseDidSuffix=0\r\n";
            text += L"PipeSuffixMode=number\r\n";
            text += L"ForceSetServiceFallback=1\r\n";
            text += L"BufferLength=20000\r\n";
            text += L"SendBufferPackets=256\r\n";
            text += L"PipeName=\\\\.\\pipe\\TVTestBridge_{TUNER}\r\n";
            text += L"TsPipeName=\\\\.\\pipe\\TVTestBridgeTS_{TUNER}\r\n";

            const unsigned char bom[2] = { 0xFF, 0xFE };

            DWORD written = 0;

            BOOL ok = ::WriteFile(
                hFile,
                bom,
                static_cast<DWORD>(sizeof(bom)),
                &written,
                nullptr);

            if (!ok || written != sizeof(bom)) {
                ::CloseHandle(hFile);
                return false;
            }

            ok = ::WriteFile(
                hFile,
                text.data(),
                static_cast<DWORD>(text.size() * sizeof(wchar_t)),
                &written,
                nullptr);

            ::CloseHandle(hFile);

            return ok != FALSE;
        }

        void EnsureDefaultIniFile_()
        {
            if (m_iniPath.empty()) {
                return;
            }

            const DWORD attr = ::GetFileAttributesW(m_iniPath.c_str());

            if (attr != INVALID_FILE_ATTRIBUTES) {
                return;
            }

            const bool ok = WriteDefaultIniFile_();

            if (ok) {
                Log(std::wstring(L"TunerBridgePlugin.ini を自動生成しました: ") + m_iniPath);
            }
            else {
                Log(std::wstring(L"TunerBridgePlugin.ini の自動生成に失敗しました: ") + m_iniPath);
            }
        }

        void LogIniFileProbe_()
        {
            DWORD attr = ::GetFileAttributesW(m_iniPath.c_str());

            std::wstringstream ss;
            ss << L"INIファイル確認: path=" << m_iniPath
                << L" exists=" << (attr != INVALID_FILE_ATTRIBUTES ? 1 : 0);

            if (attr != INVALID_FILE_ATTRIBUTES) {
                HANDLE hFile = ::CreateFileW(
                    m_iniPath.c_str(),
                    GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr);

                if (hFile != INVALID_HANDLE_VALUE) {
                    LARGE_INTEGER size{};

                    if (::GetFileSizeEx(hFile, &size)) {
                        ss << L" size=" << size.QuadPart;
                    }

                    unsigned char head[16]{};
                    DWORD readBytes = 0;

                    if (::ReadFile(
                        hFile,
                        head,
                        static_cast<DWORD>(sizeof(head)),
                        &readBytes,
                        nullptr)) {
                        ss << L" head=";

                        for (DWORD i = 0; i < readBytes; ++i) {
                            wchar_t b[8]{};
                            ::wsprintfW(
                                b,
                                L"%02X",
                                static_cast<unsigned>(head[i]));

                            ss << b;
                        }
                    }

                    ::CloseHandle(hFile);
                }
                else {
                    ss << L" openError=" << ::GetLastError();
                }
            }
            else {
                ss << L" attrError=" << ::GetLastError();
            }

            Log(ss.str());
        }

        std::wstring GetIniString(
            const wchar_t* section,
            const wchar_t* key,
            const wchar_t* def) const
        {
            std::wstring manualValue;

            if (TryReadIniStringManual_(section, key, manualValue)) {
                return manualValue;
            }

            wchar_t buf[1024]{};

            ::GetPrivateProfileStringW(
                section,
                key,
                def != nullptr ? def : L"",
                buf,
                static_cast<DWORD>(std::size(buf)),
                m_iniPath.c_str());

            return buf;
        }

        DWORD GetIniDword(
            const wchar_t* section,
            const wchar_t* key,
            DWORD def) const
        {
            std::wstring manualValue;

            if (TryReadIniStringManual_(section, key, manualValue)) {
                manualValue = StripBomAndTrim_(manualValue);

                if (!manualValue.empty()) {
                    wchar_t* endPtr = nullptr;

                    const unsigned long v =
                        std::wcstoul(manualValue.c_str(), &endPtr, 10);

                    if (endPtr != manualValue.c_str()) {
                        return static_cast<DWORD>(v);
                    }
                }
            }

            return ::GetPrivateProfileIntW(
                section,
                key,
                static_cast<INT>(def),
                m_iniPath.c_str());
        }

        void LoadSettings()
        {
            PluginSettings s;

            LogIniFileProbe_();

            if (m_settings.verboseLog || s.verboseLog) {
                std::wstring probeUseDidSuffix;
                std::wstring probePipeName;
                std::wstring probeTsPipeName;

                const bool hasUseDidSuffix =
                    TryReadIniStringManual_(L"Settings", L"UseDidSuffix", probeUseDidSuffix);

                const bool hasPipeName =
                    TryReadIniStringManual_(L"Settings", L"PipeName", probePipeName);

                const bool hasTsPipeName =
                    TryReadIniStringManual_(L"Settings", L"TsPipeName", probeTsPipeName);

                std::wstringstream ss;
                ss << L"INI手動読込確認: path=" << m_iniPath
                    << L" UseDidSuffixFound=" << (hasUseDidSuffix ? 1 : 0)
                    << L" UseDidSuffix=" << probeUseDidSuffix
                    << L" PipeNameFound=" << (hasPipeName ? 1 : 0)
                    << L" PipeName=" << probePipeName
                    << L" TsPipeNameFound=" << (hasTsPipeName ? 1 : 0)
                    << L" TsPipeName=" << probeTsPipeName;

                Log(ss.str());
            }

            s.bridgeEnabled =
                ParseBool(
                    GetIniString(L"Settings", L"Enabled", s.bridgeEnabled ? L"1" : L"0"),
                    s.bridgeEnabled);

            s.verboseLog =
                ParseBool(
                    GetIniString(L"Settings", L"VerboseLog", s.verboseLog ? L"1" : L"0"),
                    s.verboseLog);

            s.autoEnableTs =
                ParseBool(
                    GetIniString(L"Settings", L"AutoEnableTs", s.autoEnableTs ? L"1" : L"0"),
                    s.autoEnableTs);

            s.sendStatusOnConnect =
                ParseBool(
                    GetIniString(L"Settings", L"SendStatusOnConnect", s.sendStatusOnConnect ? L"1" : L"0"),
                    s.sendStatusOnConnect);

            s.useDidSuffix =
                ParseBool(
                    GetIniString(L"Settings", L"UseDidSuffix", s.useDidSuffix ? L"1" : L"0"),
                    s.useDidSuffix);

            s.forceSetServiceFallback =
                ParseBool(
                    GetIniString(L"Settings", L"ForceSetServiceFallback", s.forceSetServiceFallback ? L"1" : L"0"),
                    s.forceSetServiceFallback);

            s.bufferPackets =
                ClampDword(
                    GetIniDword(L"Settings", L"BufferLength", s.bufferPackets),
                    kMinBufferPackets,
                    kMaxBufferPackets);

            s.sendPackets =
                ClampDword(
                    GetIniDword(L"Settings", L"SendBufferPackets", s.sendPackets),
                    1,
                    4096);

            bool usedTemplate = false;
            bool usedDeviceSection = false;

            std::wstring controlPipe =
                Trim(GetIniString(L"Settings", L"PipeName", s.controlPipeName.c_str()));

            std::wstring tsPipe =
                Trim(GetIniString(L"Settings", L"TsPipeName", s.tsPipeName.c_str()));

            if (!controlPipe.empty()) {
                if (HasPipePlaceholder_(controlPipe)) {
                    controlPipe = ResolvePipeTemplate_(controlPipe, m_deviceId);
                    usedTemplate = true;
                }

                s.controlPipeName = controlPipe;
            }

            if (!tsPipe.empty()) {
                if (HasPipePlaceholder_(tsPipe)) {
                    tsPipe = ResolvePipeTemplate_(tsPipe, m_deviceId);
                    usedTemplate = true;
                }

                s.tsPipeName = tsPipe;
            }

            const auto applySection =
                [this, &s, &usedTemplate, &usedDeviceSection](const std::wstring& section)
                {
                    if (section.empty()) {
                        return;
                    }

                    const std::wstring pipe =
                        ReadPipeValueFromSection_(section, L"PipeName");

                    if (!pipe.empty()) {
                        const bool hasTemplate = HasPipePlaceholder_(pipe);

                        s.controlPipeName =
                            hasTemplate
                            ? ResolvePipeTemplate_(pipe, m_deviceId)
                            : pipe;

                        usedTemplate = usedTemplate || hasTemplate;
                        usedDeviceSection = true;
                    }

                    const std::wstring tvtestPipe =
                        ReadPipeValueFromSection_(section, L"TvTestControlPipeName");

                    if (!tvtestPipe.empty()) {
                        const bool hasTemplate = HasPipePlaceholder_(tvtestPipe);

                        s.controlPipeName =
                            hasTemplate
                            ? ResolvePipeTemplate_(tvtestPipe, m_deviceId)
                            : tvtestPipe;

                        usedTemplate = usedTemplate || hasTemplate;
                        usedDeviceSection = true;
                    }

                    const std::wstring ts =
                        ReadPipeValueFromSection_(section, L"TsPipeName");

                    if (!ts.empty()) {
                        const bool hasTemplate = HasPipePlaceholder_(ts);

                        s.tsPipeName =
                            hasTemplate
                            ? ResolvePipeTemplate_(ts, m_deviceId)
                            : ts;

                        usedTemplate = usedTemplate || hasTemplate;
                        usedDeviceSection = true;
                    }

                    const std::wstring tvtestTs =
                        ReadPipeValueFromSection_(section, L"TvTestTsPipeName");

                    if (!tvtestTs.empty()) {
                        const bool hasTemplate = HasPipePlaceholder_(tvtestTs);

                        s.tsPipeName =
                            hasTemplate
                            ? ResolvePipeTemplate_(tvtestTs, m_deviceId)
                            : tvtestTs;

                        usedTemplate = usedTemplate || hasTemplate;
                        usedDeviceSection = true;
                    }
                };

            if (m_deviceId >= 0) {
                applySection(GetDidLetterSectionName_());
                applySection(GetDidNumberSectionName_());
                applySection(GetTunerSectionName_());
            }

            if (s.useDidSuffix && m_deviceId >= 0 && !usedTemplate && !usedDeviceSection) {
                std::wstring suffixMode =
                    ToUpperCopy(
                        Trim(GetIniString(L"Settings", L"PipeSuffixMode", L"number")));

                std::wstring suffix;

                if (suffixMode == L"LETTER" ||
                    suffixMode == L"ALPHA" ||
                    suffixMode == L"A") {
                    wchar_t buf[16]{};
                    ::wsprintfW(buf, L"_%c", L'A' + m_deviceId);
                    suffix = buf;
                }
                else {
                    suffix = L"_" + std::to_wstring(m_deviceId);
                }

                AppendSuffixIfMissing_(s.controlPipeName, suffix);
                AppendSuffixIfMissing_(s.tsPipeName, suffix);
            }

            m_settings = s;
            m_tsForwardEnabled = s.autoEnableTs;

            if (m_settings.verboseLog) {
                std::wstringstream ss;

                ss << L"設定を読み込みました。"
                    << L" Enabled=" << (m_settings.bridgeEnabled ? 1 : 0)
                    << L", DID=" << m_deviceId
                    << L", UseDidSuffix=" << (m_settings.useDidSuffix ? 1 : 0)
                    << L", Template=" << (usedTemplate ? 1 : 0)
                    << L", DeviceSection=" << (usedDeviceSection ? 1 : 0)
                    << L", BufferLength=" << m_settings.bufferPackets
                    << L", SendBufferPackets=" << m_settings.sendPackets
                    << L", PipeName=" << m_settings.controlPipeName
                    << L", TsPipeName=" << m_settings.tsPipeName;

                Log(ss.str());

                if (m_deviceId < 0) {
                    Log(L"警告: /DID が指定されていないため、{TUNER} は 0 として扱われます。多重起動では /DID A/B または /DID 0/1 を指定してください。");
                }
            }
        }

        void AllocateTsBuffer()
        {
            m_streamBufferPackets = m_settings.bufferPackets;
            m_sendPackets = m_settings.sendPackets;
            m_tsBuffer.assign(static_cast<size_t>(m_streamBufferPackets) * 188, 0);
            m_streamBufferUsed = 0;
            m_streamBufferPos = 0;

            if (!m_hTsWakeEvent) {
                m_hTsWakeEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            }
        }

        void PurgeTsBuffer()
        {
            AutoLock lock(m_tsLock);
            m_streamBufferUsed = 0;
            m_streamBufferPos = 0;
        }

        bool PopTsChunk(std::vector<uint8_t>& out)
        {
            AutoLock lock(m_tsLock);

            if (m_streamBufferUsed == 0) {
                return false;
            }

            const DWORD packets =
                std::min<DWORD>(m_sendPackets, m_streamBufferUsed);

            out.resize(static_cast<size_t>(packets) * 188);

            for (DWORD i = 0; i < packets; ++i) {
                const size_t srcIndex =
                    (m_streamBufferPos + i) % m_streamBufferPackets;

                ::CopyMemory(
                    out.data() + (static_cast<size_t>(i) * 188),
                    m_tsBuffer.data() + (srcIndex * 188),
                    188);
            }

            m_streamBufferPos =
                (m_streamBufferPos + packets) % m_streamBufferPackets;

            m_streamBufferUsed -= packets;
            return true;
        }

        void CreateHiddenWindow()
        {
            wchar_t className[64]{};
            ::wsprintfW(className, L"TVTestBridgePluginWnd_%p", this);
            m_windowClassName = className;

            WNDCLASSW wc{};
            wc.lpfnWndProc = HiddenWndProc;
            wc.hInstance = g_hinstDLL;
            wc.lpszClassName = m_windowClassName.c_str();

            ::RegisterClassW(&wc);

            m_hiddenWnd = ::CreateWindowExW(
                0,
                m_windowClassName.c_str(),
                L"TVTestBridgeHiddenWindow",
                0,
                0,
                0,
                0,
                0,
                HWND_MESSAGE,
                nullptr,
                g_hinstDLL,
                this);
        }

        void DisconnectPipeHandle(HANDLE& h)
        {
            if (h != INVALID_HANDLE_VALUE && h != nullptr) {
                ::FlushFileBuffers(h);
                ::DisconnectNamedPipe(h);
                ::CloseHandle(h);
                h = INVALID_HANDLE_VALUE;
            }
        }

        void WakeTsSender()
        {
            if (m_hTsWakeEvent) {
                ::SetEvent(m_hTsWakeEvent);
            }
        }

        bool HasTsClient()
        {
            AutoLock lock(m_tsPipeWriteLock);
            return m_tsPipeClient != INVALID_HANDLE_VALUE && m_tsPipeClient != nullptr;
        }

        DWORD GetBufferedPacketCount()
        {
            AutoLock lock(m_tsLock);
            return m_streamBufferUsed;
        }

        HANDLE CreateListener(const std::wstring& pipeName)
        {
            HANDLE h = ::CreateNamedPipeW(
                pipeName.c_str(),
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                1,
                64 * 1024,
                64 * 1024,
                1000,
                nullptr);

            if (h == INVALID_HANDLE_VALUE && m_settings.verboseLog) {
                const DWORD err = ::GetLastError();

                std::wstringstream ss;
                ss << L"CreateNamedPipeW に失敗しました。PipeName=" << pipeName
                    << L" GetLastError=" << err;

                Log(ss.str());
            }
            else if (m_settings.verboseLog) {
                std::wstringstream ss;
                ss << L"CreateNamedPipeW 成功。PipeName=" << pipeName;
                Log(ss.str());
            }

            return h;
        }

        void ControlPipeThread()
        {
            while (m_running) {
                HANDLE listener = CreateListener(m_settings.controlPipeName);

                if (listener == INVALID_HANDLE_VALUE) {
                    ::Sleep(kPipeReconnectSleepMs);
                    continue;
                }

                m_controlPipeListener = listener;

                const BOOL connected =
                    ::ConnectNamedPipe(listener, nullptr)
                    ? TRUE
                    : (::GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);

                if (!m_running) {
                    ::CloseHandle(listener);
                    m_controlPipeListener = INVALID_HANDLE_VALUE;
                    break;
                }

                if (!connected) {
                    ::CloseHandle(listener);
                    m_controlPipeListener = INVALID_HANDLE_VALUE;
                    ::Sleep(kPipeReconnectSleepMs);
                    continue;
                }

                {
                    AutoLock lock(m_controlPipeWriteLock);
                    m_controlPipeClient = listener;
                }

                m_controlPipeListener = INVALID_HANDLE_VALUE;

                NotifyLine("HELLO version=1");

                if (m_settings.sendStatusOnConnect) {
                    SnapshotAndNotify(L"CONNECTED");
                }

                ReadControlCommands(listener);

                {
                    AutoLock lock(m_controlPipeWriteLock);
                    if (m_controlPipeClient == listener) {
                        m_controlPipeClient = INVALID_HANDLE_VALUE;
                    }
                }

                ::DisconnectNamedPipe(listener);
                ::CloseHandle(listener);
            }
        }

        void TsPipeThread()
        {
            while (m_running) {
                HANDLE listener = CreateListener(m_settings.tsPipeName);

                if (listener == INVALID_HANDLE_VALUE) {
                    if (m_settings.verboseLog) {
                        Log(L"TS転送パイプリスナー作成に失敗しました。");
                    }

                    ::Sleep(kPipeReconnectSleepMs);
                    continue;
                }

                m_tsPipeListener = listener;

                const BOOL connected =
                    ::ConnectNamedPipe(listener, nullptr)
                    ? TRUE
                    : (::GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);

                if (!m_running) {
                    ::CloseHandle(listener);
                    m_tsPipeListener = INVALID_HANDLE_VALUE;
                    break;
                }

                if (!connected) {
                    if (m_settings.verboseLog) {
                        const DWORD err = ::GetLastError();

                        std::wstringstream ss;
                        ss << L"TS転送パイプ接続待ち失敗。err=" << err;

                        Log(ss.str());
                    }

                    ::CloseHandle(listener);
                    m_tsPipeListener = INVALID_HANDLE_VALUE;
                    ::Sleep(kPipeReconnectSleepMs);
                    continue;
                }

                {
                    AutoLock lock(m_tsPipeWriteLock);
                    m_tsPipeClient = listener;
                }

                m_tsPipeListener = INVALID_HANDLE_VALUE;

                if (m_settings.verboseLog) {
                    std::wstringstream ss;
                    ss << L"TS転送パイプにクライアントが接続しました。"
                        << L" buffered=" << GetBufferedPacketCount()
                        << L" tsEnabled=" << (m_tsForwardEnabled ? 1 : 0);

                    Log(ss.str());
                }

                WakeTsSender();

                while (m_running) {
                    if (m_tsPipeClient != listener) {
                        break;
                    }

                    ::Sleep(100);
                }

                {
                    AutoLock lock(m_tsPipeWriteLock);
                    if (m_tsPipeClient == listener) {
                        m_tsPipeClient = INVALID_HANDLE_VALUE;
                    }
                }

                ::DisconnectNamedPipe(listener);
                ::CloseHandle(listener);

                if (m_settings.verboseLog) {
                    Log(L"TS転送パイプのクライアント接続が切断されました。");
                }
            }
        }

        void ReadControlCommands(HANDLE pipe)
        {
            std::string pending;
            char buffer[2048];

            while (m_running) {
                DWORD bytesRead = 0;

                const BOOL ok =
                    ::ReadFile(pipe, buffer, sizeof(buffer), &bytesRead, nullptr);

                if (!ok || bytesRead == 0) {
                    break;
                }

                pending.append(buffer, buffer + bytesRead);

                size_t pos = 0;

                while (true) {
                    const size_t eol = pending.find('\n', pos);

                    if (eol == std::string::npos) {
                        pending.erase(0, pos);
                        break;
                    }

                    std::string line = pending.substr(pos, eol - pos);
                    pos = eol + 1;

                    while (!line.empty() &&
                        (line.back() == '\r' || line.back() == '\n')) {
                        line.pop_back();
                    }

                    if (line.empty()) {
                        continue;
                    }

                    const std::string response = ExecuteCommandViaUiThread(line);

                    if (!response.empty()) {
                        WriteControlLineUnlocked(response);
                    }
                }
            }

            if (m_settings.verboseLog) {
                Log(L"制御パイプのクライアント接続が切断されました。");
            }
        }

        std::string ExecuteCommandViaUiThread(const std::string& line)
        {
            if (!m_hiddenWnd) {
                return "ERR hidden window not ready";
            }

            auto* raw = new BridgeCommand();
            raw->line = line;
            raw->doneEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

            if (!raw->doneEvent) {
                delete raw;
                return "ERR CreateEvent failed";
            }

            if (!::PostMessageW(
                m_hiddenWnd,
                WM_APP_BRIDGE_EXEC,
                0,
                reinterpret_cast<LPARAM>(raw))) {
                ::CloseHandle(raw->doneEvent);
                delete raw;
                return "ERR failed to post command";
            }

            const DWORD waitRes =
                ::WaitForSingleObject(raw->doneEvent, kCommandWaitMs);

            std::string response =
                (waitRes == WAIT_OBJECT_0)
                ? raw->response
                : "ERR command timeout";

            ::CloseHandle(raw->doneEvent);
            delete raw;

            return response;
        }

        struct ChannelResolveResult
        {
            int space = -1;
            int channelIndex = -1;
            WORD serviceId = 0;
            WORD networkId = 0;
            WORD transportStreamId = 0;
        };

        bool IsValidTuningSpace(int space) const
        {
            wchar_t name[256]{};

            return m_pApp->GetTuningSpaceName(
                space,
                name,
                static_cast<int>(std::size(name))) > 0;
        }

        bool FindChannelByIds(
            int preferredSpace,
            WORD networkId,
            WORD transportStreamId,
            WORD serviceId,
            ChannelResolveResult& out)
        {
            out = ChannelResolveResult{};

            auto matchInSpace = [&](int space) -> bool
                {
                    int missCount = 0;

                    for (int index = 0; index < 512; ++index) {
                        TVTest::ChannelInfo info{};

                        if (!m_pApp->GetChannelInfo(space, index, &info)) {
                            ++missCount;

                            if (missCount >= 8) {
                                break;
                            }

                            continue;
                        }

                        missCount = 0;

                        if (info.NetworkID != networkId) {
                            continue;
                        }

                        if (info.TransportStreamID != transportStreamId) {
                            continue;
                        }

                        if (serviceId != 0 && info.ServiceID != serviceId) {
                            continue;
                        }

                        out.space = space;
                        out.channelIndex = index;
                        out.serviceId = info.ServiceID;
                        out.networkId = info.NetworkID;
                        out.transportStreamId = info.TransportStreamID;

                        return true;
                    }

                    return false;
                };

            if (preferredSpace >= 0 && IsValidTuningSpace(preferredSpace)) {
                if (matchInSpace(preferredSpace)) {
                    return true;
                }
            }

            for (int space = 0; space < 16; ++space) {
                if (space == preferredSpace) {
                    continue;
                }

                if (!IsValidTuningSpace(space)) {
                    continue;
                }

                if (matchInSpace(space)) {
                    return true;
                }
            }

            return false;
        }

        bool SetChannelByResolvedIds(
            int preferredSpace,
            WORD networkId,
            WORD transportStreamId,
            WORD serviceId)
        {
            ChannelResolveResult resolved{};

            if (!FindChannelByIds(
                preferredSpace,
                networkId,
                transportStreamId,
                serviceId,
                resolved)) {
                if (m_settings.verboseLog) {
                    std::wstringstream ss;

                    ss << L"SET_CHANNEL_BY_IDS: channel not found. "
                        << L"space=" << preferredSpace
                        << L" nid=" << networkId
                        << L" tsid=" << transportStreamId
                        << L" sid=" << serviceId;

                    Log(ss.str());
                }

                return false;
            }

            const bool ok =
                m_pApp->SetChannel(
                    resolved.space,
                    resolved.channelIndex,
                    resolved.serviceId);

            if (ok && m_settings.forceSetServiceFallback && resolved.serviceId != 0) {
                const int svcIndex = m_pApp->GetService();
                TVTest::ServiceInfo info{};

                if (svcIndex < 0 ||
                    !m_pApp->GetServiceInfo(svcIndex, &info) ||
                    info.ServiceID != resolved.serviceId) {
                    m_pApp->SetService(resolved.serviceId, true);
                }
            }

            if (m_settings.verboseLog) {
                std::wstringstream ss;

                ss << L"SET_CHANNEL_BY_IDS resolved: "
                    << L"reqSpace=" << preferredSpace
                    << L" -> space=" << resolved.space
                    << L" index=" << resolved.channelIndex
                    << L" sid=" << resolved.serviceId
                    << L" nid=" << resolved.networkId
                    << L" tsid=" << resolved.transportStreamId
                    << L" result=" << (ok ? L"OK" : L"NG");

                Log(ss.str());
            }

            return ok;
        }

        std::string ExecuteCommandOnUiThread(const std::string& line)
        {
            const auto args = SplitArgs(line);

            if (args.empty()) {
                return "ERR empty command";
            }

            std::string cmd = args[0];

            std::transform(
                cmd.begin(),
                cmd.end(),
                cmd.begin(),
                [](unsigned char ch) {
                    return static_cast<char>(std::toupper(ch));
                });

            if (cmd == "PING") {
                return "OK PONG";
            }

            if (cmd == "GET_STATE") {
                return BuildStateLine();
            }

            if (cmd == "GET_STATUS") {
                return BuildStatusLine();
            }

            if (cmd == "GET_PROGRAM") {
                return BuildProgramLine();
            }

            if (cmd == "ENABLE_TS") {
                m_tsForwardEnabled = true;
                WakeTsSender();

                if (m_settings.verboseLog) {
                    std::wstringstream ss;

                    ss << L"ENABLE_TS accepted: buffered=" << GetBufferedPacketCount()
                        << L" packets, tsClient=" << (HasTsClient() ? L"connected" : L"none");

                    Log(ss.str());
                }

                return "OK TS enabled";
            }

            if (cmd == "DISABLE_TS") {
                m_tsForwardEnabled = false;

                if (m_settings.verboseLog) {
                    Log(L"DISABLE_TS accepted.");
                }

                return "OK TS disabled";
            }

            if (cmd == "SET_CHANNEL") {
                if (args.size() < 3) {
                    return "ERR usage: SET_CHANNEL <space> <channel> [serviceId]";
                }

                const int space = std::atoi(args[1].c_str());
                const int channel = std::atoi(args[2].c_str());

                const WORD serviceId =
                    (args.size() >= 4)
                    ? static_cast<WORD>(std::strtoul(args[3].c_str(), nullptr, 0))
                    : 0;

                const bool ok = m_pApp->SetChannel(space, channel, serviceId);

                if (ok && m_settings.forceSetServiceFallback && serviceId != 0) {
                    const int svcIndex = m_pApp->GetService();
                    TVTest::ServiceInfo info{};

                    if (svcIndex < 0 ||
                        !m_pApp->GetServiceInfo(svcIndex, &info) ||
                        info.ServiceID != serviceId) {
                        m_pApp->SetService(serviceId, true);
                    }
                }

                SnapshotAndNotify(L"SET_CHANNEL");
                return ok ? "OK" : "ERR SetChannel failed";
            }

            if (cmd == "SET_CHANNEL_INDEX") {
                if (args.size() < 3) {
                    return "ERR usage: SET_CHANNEL_INDEX <space> <index> [serviceId]";
                }

                const int space = std::atoi(args[1].c_str());
                const int index = std::atoi(args[2].c_str());

                const WORD serviceId =
                    (args.size() >= 4)
                    ? static_cast<WORD>(std::strtoul(args[3].c_str(), nullptr, 0))
                    : 0;

                const bool ok = m_pApp->SetChannel(space, index, serviceId);

                if (ok && m_settings.forceSetServiceFallback && serviceId != 0) {
                    const int svcIndex = m_pApp->GetService();
                    TVTest::ServiceInfo info{};

                    if (svcIndex < 0 ||
                        !m_pApp->GetServiceInfo(svcIndex, &info) ||
                        info.ServiceID != serviceId) {
                        m_pApp->SetService(serviceId, true);
                    }
                }

                SnapshotAndNotify(L"SET_CHANNEL_INDEX");
                return ok ? "OK" : "ERR SetChannelIndex failed";
            }

            if (cmd == "SET_CHANNEL_BY_IDS") {
                if (args.size() < 5) {
                    return "ERR usage: SET_CHANNEL_BY_IDS <space> <nid> <tsid> <sid>";
                }

                const int preferredSpace = std::atoi(args[1].c_str());

                const WORD networkId =
                    static_cast<WORD>(std::strtoul(args[2].c_str(), nullptr, 0));

                const WORD transportStreamId =
                    static_cast<WORD>(std::strtoul(args[3].c_str(), nullptr, 0));

                const WORD serviceId =
                    static_cast<WORD>(std::strtoul(args[4].c_str(), nullptr, 0));

                const bool ok =
                    SetChannelByResolvedIds(
                        preferredSpace,
                        networkId,
                        transportStreamId,
                        serviceId);

                SnapshotAndNotify(L"SET_CHANNEL_BY_IDS");
                return ok ? "OK" : "ERR SetChannelByIds failed";
            }

            if (cmd == "SET_SERVICE") {
                if (args.size() < 2) {
                    return "ERR usage: SET_SERVICE <serviceId>";
                }

                const WORD serviceId =
                    static_cast<WORD>(std::strtoul(args[1].c_str(), nullptr, 0));

                const bool ok = m_pApp->SetService(serviceId, true);

                SnapshotAndNotify(L"SET_SERVICE");
                return ok ? "OK" : "ERR SetService failed";
            }

            if (cmd == "START_RECORD") {
                bool ok = false;

                if (args.size() >= 2) {
                    const std::wstring path = JoinWide(args, 1);

                    TVTest::RecordInfo rec{};
                    rec.Mask = TVTest::RECORD_MASK_FILENAME;
                    rec.Flags = 0;
                    rec.pszFileName = const_cast<LPWSTR>(path.c_str());
                    rec.StartTimeSpec = TVTest::RECORD_START_NOTSPECIFIED;
                    rec.StopTimeSpec = TVTest::RECORD_STOP_NOTSPECIFIED;

                    ok = m_pApp->StartRecord(&rec);
                }
                else {
                    ok = m_pApp->StartRecord(nullptr);
                }

                SnapshotAndNotify(L"START_RECORD_CMD");
                return ok ? "OK" : "ERR StartRecord failed";
            }

            if (cmd == "STOP_RECORD") {
                const bool ok = m_pApp->StopRecord();

                SnapshotAndNotify(L"STOP_RECORD_CMD");
                return ok ? "OK" : "ERR StopRecord failed";
            }

            if (cmd == "DO_COMMAND") {
                if (args.size() < 2) {
                    return "ERR usage: DO_COMMAND <commandName>";
                }

                const std::wstring wcmd = JoinWide(args, 1);
                const bool ok = m_pApp->DoCommand(wcmd.c_str());

                return ok ? "OK" : "ERR DoCommand failed";
            }

            if (cmd == "SET_SILENT") {
                if (args.size() < 2) {
                    return "ERR usage: SET_SILENT <0|1>";
                }

                const bool ok =
                    m_pApp->SetSilentMode(std::atoi(args[1].c_str()) != 0);

                return ok ? "OK" : "ERR SetSilentMode failed";
            }

            if (cmd == "SET_PREVIEW") {
                if (args.size() < 2) {
                    return "ERR usage: SET_PREVIEW <0|1>";
                }

                const bool ok =
                    m_pApp->SetPreview(std::atoi(args[1].c_str()) != 0);

                return ok ? "OK" : "ERR SetPreview failed";
            }

            if (cmd == "CLOSE") {
                const bool ok = m_pApp->Close(TVTest::CLOSE_EXIT);

                return ok ? "OK" : "ERR Close failed";
            }

            if (cmd == "RELOAD_INI") {
                LoadSettings();
                return "OK";
            }

            return "ERR unknown command";
        }

        std::string BuildSimpleEventLine(
            const std::wstring& kind,
            const std::wstring& extra)
        {
            std::wstringstream ss;

            ss << L"EVT kind=\"" << kind << L"\"";

            if (!extra.empty()) {
                ss << L" value=\"" << extra << L"\"";
            }

            return WideToUtf8(ss.str());
        }

        void SnapshotAndNotify(const std::wstring& reason)
        {
            NotifyLine(BuildSnapshotLine(CaptureSnapshot(), reason));
        }

        StateSnapshot CaptureSnapshot()
        {
            StateSnapshot s;
            s.bridgeEnabled = m_settings.bridgeEnabled;
            s.deviceId = m_deviceId;
            s.driverName = GetDriverNameSafe();

            TVTest::ChannelInfo ch{};

            if (m_pApp->GetCurrentChannelInfo(&ch)) {
                s.space = ch.Space;
                s.channelIndex = ch.Channel;
                s.transportStreamId = ch.TransportStreamID;
                s.networkId = ch.NetworkID;
                s.serviceId = ch.ServiceID;
            }

            const int serviceIndex = m_pApp->GetService();
            TVTest::ServiceInfo svc{};

            if (serviceIndex >= 0 && m_pApp->GetServiceInfo(serviceIndex, &svc)) {
                s.serviceId = svc.ServiceID;
            }

            TVTest::StatusInfo status{};

            if (m_pApp->GetStatus(&status)) {
                s.signalLevel = status.SignalLevel;
                s.bitRate = status.BitRate;
                s.errorPackets = status.ErrorPacketCount;
                s.scramblePackets = status.ScramblePacketCount;

#if TVTEST_PLUGIN_VERSION >= TVTEST_PLUGIN_VERSION_(0,0,2)
                s.dropPackets = status.DropPacketCount;
#endif
            }

            TVTest::RecordStatusInfo rec{};

#if TVTEST_PLUGIN_VERSION >= TVTEST_PLUGIN_VERSION_(0,0,10)
            rec.pszFileName = nullptr;
            rec.MaxFileName = 0;
#endif

            if (m_pApp->GetRecordStatus(&rec)) {
                s.recordStatus = rec.Status;
            }

            TVTest::ProgramInfo progNow{};
            wchar_t eventNow[256]{};
            wchar_t textNow[1]{};
            wchar_t extNow[1]{};

            progNow.pszEventName = eventNow;
            progNow.MaxEventName = static_cast<int>(std::size(eventNow));
            progNow.pszEventText = textNow;
            progNow.MaxEventText = 0;
            progNow.pszEventExtText = extNow;
            progNow.MaxEventExtText = 0;

            if (m_pApp->GetCurrentProgramInfo(&progNow, false)) {
                s.eventIdNow = progNow.EventID;
                s.eventNameNow = eventNow;
            }

            TVTest::ProgramInfo progNext{};
            wchar_t eventNext[256]{};
            wchar_t textNext[1]{};
            wchar_t extNext[1]{};

            progNext.pszEventName = eventNext;
            progNext.MaxEventName = static_cast<int>(std::size(eventNext));
            progNext.pszEventText = textNext;
            progNext.MaxEventText = 0;
            progNext.pszEventExtText = extNext;
            progNext.MaxEventExtText = 0;

            if (m_pApp->GetCurrentProgramInfo(&progNext, true)) {
                s.eventIdNext = progNext.EventID;
                s.eventNameNext = eventNext;
            }

            return s;
        }

        std::wstring GetDriverNameSafe()
        {
            if (!m_driverArgName.empty()) {
                return m_driverArgName;
            }

            wchar_t buf[MAX_PATH]{};

            if (m_pApp->GetDriverName(buf, MAX_PATH) > 0) {
                return buf;
            }

            return {};
        }

        std::string BuildSnapshotLine(
            const StateSnapshot& s,
            const std::wstring& reason)
        {
            std::wstringstream ss;

            ss << L"SNAP"
                << L" reason=\"" << reason << L"\""
                << L" bridge=" << (s.bridgeEnabled ? 1 : 0)
                << L" did=" << s.deviceId
                << L" space=" << s.space
                << L" channel=" << s.channelIndex
                << L" sid=" << s.serviceId
                << L" tsid=" << s.transportStreamId
                << L" nid=" << s.networkId
                << L" rec=" << s.recordStatus
                << L" signal=" << s.signalLevel
                << L" bitrate=" << s.bitRate
                << L" err=" << s.errorPackets
                << L" scramble=" << s.scramblePackets
                << L" drop=" << s.dropPackets
                << L" eid_now=" << s.eventIdNow
                << L" eid_next=" << s.eventIdNext
                << L" driver=\"" << s.driverName << L"\""
                << L" event_now=\"" << s.eventNameNow << L"\""
                << L" event_next=\"" << s.eventNameNext << L"\"";

            return WideToUtf8(ss.str());
        }

        std::string BuildStateLine()
        {
            return BuildSnapshotLine(CaptureSnapshot(), L"GET_STATE");
        }

        std::string BuildStatusLine()
        {
            const StateSnapshot s = CaptureSnapshot();

            std::wstringstream ss;

            ss << L"STATUS signal=" << s.signalLevel
                << L" bitrate=" << s.bitRate
                << L" err=" << s.errorPackets
                << L" scramble=" << s.scramblePackets
                << L" drop=" << s.dropPackets
                << L" rec=" << s.recordStatus;

            return WideToUtf8(ss.str());
        }

        std::string BuildProgramLine()
        {
            const StateSnapshot s = CaptureSnapshot();

            std::wstringstream ss;

            ss << L"PROGRAM sid=" << s.serviceId
                << L" eid_now=" << s.eventIdNow
                << L" event_now=\"" << s.eventNameNow << L"\""
                << L" eid_next=" << s.eventIdNext
                << L" event_next=\"" << s.eventNameNext << L"\"";

            return WideToUtf8(ss.str());
        }

        bool WriteControlLineUnlocked(const std::string& line)
        {
            HANDLE pipe = INVALID_HANDLE_VALUE;

            {
                AutoLock lock(m_controlPipeWriteLock);
                pipe = m_controlPipeClient;
            }

            if (pipe == INVALID_HANDLE_VALUE || pipe == nullptr) {
                return false;
            }

            std::string data = line;
            data.append("\r\n");

            DWORD written = 0;

            return
                ::WriteFile(
                    pipe,
                    data.data(),
                    static_cast<DWORD>(data.size()),
                    &written,
                    nullptr) &&
                written == data.size();
        }

        void NotifyLine(const std::string& line)
        {
            if (!line.empty()) {
                WriteControlLineUnlocked(line);
            }
        }

        void TsSenderThread()
        {
            std::vector<uint8_t> chunk;
            DWORD idleLogCounter = 0;

            while (m_running) {
                if (m_hTsWakeEvent) {
                    ::WaitForSingleObject(m_hTsWakeEvent, 100);
                }
                else {
                    ::Sleep(100);
                }

                if (!m_running) {
                    break;
                }

                if (!m_tsForwardEnabled) {
                    continue;
                }

                if (!HasTsClient()) {
                    if (m_settings.verboseLog && ((++idleLogCounter % 50) == 1)) {
                        Log(L"TS送信待機中: TSクライアント未接続。");
                    }

                    continue;
                }

                bool sentAny = false;

                while (m_running && m_tsForwardEnabled && PopTsChunk(chunk)) {
                    sentAny = true;
                    SendTsChunk(chunk);

                    if (!HasTsClient()) {
                        break;
                    }
                }

                if (!sentAny) {
                    if (m_settings.verboseLog && ((++idleLogCounter % 50) == 1)) {
                        std::wstringstream ss;
                        ss << L"TS送信待機中: バッファ空。buffered="
                            << GetBufferedPacketCount();

                        Log(ss.str());
                    }
                }
                else {
                    idleLogCounter = 0;
                }
            }
        }

        void SendTsChunk(const std::vector<uint8_t>& chunk)
        {
            HANDLE pipe = INVALID_HANDLE_VALUE;

            {
                AutoLock lock(m_tsPipeWriteLock);
                pipe = m_tsPipeClient;
            }

            if (pipe == INVALID_HANDLE_VALUE || pipe == nullptr) {
                return;
            }

            TsFrameHeader hdr{};
            hdr.magic = kTsFrameMagic;
            hdr.payloadBytes = static_cast<uint32_t>(chunk.size());
            hdr.flags = 0;
            hdr.reserved = 0;

            std::vector<uint8_t> frame(sizeof(TsFrameHeader) + chunk.size());

            ::CopyMemory(frame.data(), &hdr, sizeof(TsFrameHeader));

            if (!chunk.empty()) {
                ::CopyMemory(
                    frame.data() + sizeof(TsFrameHeader),
                    chunk.data(),
                    chunk.size());
            }

            DWORD written = 0;

            const BOOL ok = ::WriteFile(
                pipe,
                frame.data(),
                static_cast<DWORD>(frame.size()),
                &written,
                nullptr);

            if (!ok || written != frame.size()) {
                const DWORD err = ::GetLastError();

                if (m_settings.verboseLog) {
                    std::wstringstream ss;

                    ss << L"TS送信失敗。written=" << written
                        << L" expected=" << frame.size()
                        << L" err=" << err
                        << L" payload=" << chunk.size();

                    Log(ss.str());
                }

                DisconnectTsClient();
                return;
            }

            if (m_settings.verboseLog) {
                static std::atomic_uint32_t s_tsSendLogCount{ 0 };

                const uint32_t n = s_tsSendLogCount.fetch_add(1);

                if (n < 4) {
                    std::wstringstream ss;

                    ss << L"TS送信成功。frameBytes=" << frame.size()
                        << L" payload=" << chunk.size();

                    Log(ss.str());
                }
            }
        }

        void DisconnectTsClient()
        {
            HANDLE pipe = INVALID_HANDLE_VALUE;

            {
                AutoLock lock(m_tsPipeWriteLock);
                pipe = m_tsPipeClient;
                m_tsPipeClient = INVALID_HANDLE_VALUE;
            }

            if (pipe != INVALID_HANDLE_VALUE && pipe != nullptr) {
                ::FlushFileBuffers(pipe);
                ::DisconnectNamedPipe(pipe);
                ::CloseHandle(pipe);

                if (m_settings.verboseLog) {
                    Log(L"TS転送パイプクライアントを切断しました。");
                }
            }
        }

    private:
        std::wstring m_modulePath;
        std::wstring m_iniPath;
        PluginSettings m_settings;
        std::wstring m_driverArgName;
        int m_deviceId = -1;
        std::wstring m_windowClassName;
        HWND m_hiddenWnd = nullptr;

        std::atomic_bool m_running{ false };
        std::atomic_bool m_tsForwardEnabled{ false };
        std::atomic_bool m_pluginEnabledByHost{ true };

        std::thread m_controlThread;
        std::thread m_tsPipeThread;
        std::thread m_tsThread;

        HANDLE m_hTsWakeEvent = nullptr;
        HANDLE m_controlPipeListener = INVALID_HANDLE_VALUE;
        HANDLE m_controlPipeClient = INVALID_HANDLE_VALUE;
        HANDLE m_tsPipeListener = INVALID_HANDLE_VALUE;
        HANDLE m_tsPipeClient = INVALID_HANDLE_VALUE;

        CCriticalLock m_controlPipeWriteLock;
        CCriticalLock m_tsPipeWriteLock;
        CCriticalLock m_tsLock;

        std::vector<uint8_t> m_tsBuffer;
        DWORD m_streamBufferPackets = 0;
        DWORD m_sendPackets = 0;
        DWORD m_streamBufferUsed = 0;
        DWORD m_streamBufferPos = 0;
        DWORD m_dropCount = 0;
    };

} // namespace

TVTest::CTVTestPlugin* CreatePluginClass()
{
    return new CTvTestBridgePlugin;
}