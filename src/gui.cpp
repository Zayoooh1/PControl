#include "ipc.hpp"
#include <algorithm>
#include <atomic>
#include <commctrl.h>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
namespace {
using pc::Json;
constexpr UINT ready = WM_APP + 1;
enum {
    Processes = 100,
    Priority,
    ApplyPriority,
    SavePriority,
    ApplyAffinity,
    SaveAffinity,
    AllCpu,
    Reconnect,
    Reload,
    RemovePriority,
    RemoveAffinity,
    RuleName
};
HWND window, list, status, detail, priority, cpus, rules, ruleName;
HFONT font;
Json rows = Json::array(), configuration;
std::thread worker;
std::atomic_bool busy = false;
std::mutex resultMutex;
Json workerResult;
bool rendering = false;
bool closing = false;
bool connected = false;
struct PresentationIdentity {
    DWORD pid = 0;
    std::string creationTime;
    std::string name;
    std::string path;
};
std::optional<PresentationIdentity> selectedProcess;
const wchar_t *priorityNames[] = {L"Idle",         L"Below Normal", L"Normal",
                                  L"Above Normal", L"High",         L"Real Time"};
const DWORD priorityValues[] = {0x40, 0x4000, 0x20, 0x8000, 0x80, 0x100};
std::wstring priorityLabel(DWORD p) {
    for (int i = 0; i < 6; ++i)
        if (priorityValues[i] == p)
            return priorityNames[i];
    return L"Unknown";
}
HWND control(const wchar_t *cls, const wchar_t *text, DWORD style, int id) {
    auto h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}
std::wstring text(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring r(n + 1, 0);
    GetWindowTextW(h, r.data(), n + 1);
    r.resize(n);
    return r;
}
void launchCore() {
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(nullptr, path, 32768);
    if (!n || n >= 32768)
        throw pc::Error(ERROR_BUFFER_OVERFLOW, "GUI executable path unavailable");
    auto exe = std::filesystem::path(path).parent_path() / L"PControl.Core.exe";
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    pc::check(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                             exe.parent_path().c_str(), &si, &pi),
              "Start Core");
    pc::Handle process(pi.hProcess), thread(pi.hThread);
}
void begin(Json action = Json(), bool start = false) {
    if (busy || closing)
        return;
    if (worker.joinable())
        worker.join();
    busy = true;
    worker = std::thread([action, start] {
        Json out;
        try {
            if (start) {
                bool alive = false;
                try {
                    alive = pc::request({{"command", "Ping"}}).value("success", false);
                } catch (...) {
                }
                if (!alive) {
                    launchCore();
                    auto deadline = GetTickCount64() + 4000;
                    while (GetTickCount64() < deadline) {
                        try {
                            if (pc::request({{"command", "Ping"}}).value("success", false))
                                break;
                        } catch (...) {
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                }
            }
            if (!action.is_null())
                out["action"] = pc::request(action);
            out["status"] = pc::request({{"command", "GetStatus"}});
            out["rows"] = pc::request({{"command", "GetProcessList"}});
            out["config"] = pc::request({{"command", "GetConfiguration"}});
        } catch (const std::exception &e) {
            out["error"] = e.what();
        }
        {
            std::lock_guard lock(resultMutex);
            workerResult = std::move(out);
        }
        PostMessageW(window, ready, 0, 0);
    });
}
Json selected() {
    int index = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (index < 0 || index >= (int)rows.size())
        throw pc::Error(ERROR_INVALID_PARAMETER, "Select a process first");
    return rows[index];
}
PresentationIdentity presentationIdentity(const Json &process) {
    return {process.value("pid", DWORD{}), process.value("creationTime", ""), process.value("name", ""),
            process.value("path", "")};
}
bool samePresentationProcess(const PresentationIdentity &left, const PresentationIdentity &right) {
    if (left.pid != right.pid)
        return false;
    if (!left.creationTime.empty() && left.creationTime != "0" && !right.creationTime.empty() &&
        right.creationTime != "0")
        return left.creationTime == right.creationTime;
    return left.creationTime == "0" && right.creationTime == "0" &&
           CompareStringOrdinal(pc::wide(left.name).c_str(), -1, pc::wide(right.name).c_str(), -1, TRUE) ==
               CSTR_EQUAL &&
           CompareStringOrdinal(pc::wide(left.path).c_str(), -1, pc::wide(right.path).c_str(), -1, TRUE) ==
               CSTR_EQUAL;
}
int findProcess(const PresentationIdentity &identity) {
    for (int index = 0; index < static_cast<int>(rows.size()); ++index)
        if (samePresentationProcess(presentationIdentity(rows[index]), identity))
            return index;
    return -1;
}
bool sameStructure(const Json &oldRows, const Json &newRows) {
    if (oldRows.size() != newRows.size())
        return false;
    for (size_t index = 0; index < oldRows.size(); ++index)
        if (!samePresentationProcess(presentationIdentity(oldRows[index]),
                                     presentationIdentity(newRows[index])))
            return false;
    return true;
}
std::vector<std::wstring> cells(const Json &process) {
    return {pc::wide(process.value("name", "")),
            std::to_wstring(process.value("pid", DWORD{})),
            priorityLabel(process.value("priority", 0u)),
            pc::wide(process.value("affinityMask", "0")),
            pc::wide(process.value("architecture", "Unknown")),
            process.value("accessError", 0u)
                ? L"Win32 " + std::to_wstring(process.value("accessError", DWORD{}))
                : L"Accessible",
            pc::wide(process.value("path", ""))};
}
void updateRowsInPlace(const Json &newRows) {
    rendering = true;
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        auto before = cells(rows[row]);
        auto after = cells(newRows[row]);
        for (int column = 0; column < static_cast<int>(after.size()); ++column)
            if (before[column] != after[column])
                ListView_SetItemText(list, row, column, after[column].data());
    }
    rows = newRows;
    rendering = false;
}
void restoreTopProcess(const PresentationIdentity &identity, int fallbackIndex, int pixelOffset) {
    int count = ListView_GetItemCount(list);
    if (!count)
        return;
    int target = findProcess(identity);
    if (target < 0)
        target = std::clamp(fallbackIndex, 0, count - 1);
    int currentTop = ListView_GetTopIndex(list);
    RECT visibleRow{};
    if (!ListView_GetItemRect(list, currentTop, &visibleRow, LVIR_BOUNDS))
        return;
    int rowHeight = visibleRow.bottom - visibleRow.top;
    ListView_Scroll(list, 0, (target - currentTop) * rowHeight + visibleRow.top - pixelOffset);
    RECT restoredRow{};
    if (ListView_GetItemRect(list, target, &restoredRow, LVIR_BOUNDS) && restoredRow.top != pixelOffset)
        ListView_Scroll(list, 0, restoredRow.top - pixelOffset);
}
void selection() {
    try {
        auto p = selected();
        if (!rendering)
            selectedProcess = presentationIdentity(p);
        SetWindowTextW(detail, pc::wide(p.at("name").get<std::string>() + " | " + p.value("path", "") +
                                        " | creation=" + p.at("creationTime").get<std::string>())
                                   .c_str());
        SetWindowTextW(ruleName, pc::wide(p.at("name")).c_str());
        DWORD pr = p.value("priority", 0u);
        for (int i = 0; i < 6; ++i)
            if (priorityValues[i] == pr)
                SendMessageW(priority, CB_SETCURSEL, i, 0);
        auto mask = pc::decimal(p.value("affinityMask", "0"));
        for (int i = 0; i < ListView_GetItemCount(cpus); ++i)
            ListView_SetCheckState(cpus, i, (mask & (uint64_t(1) << i)) != 0);
    } catch (...) {
        if (!rendering)
            selectedProcess.reset();
    }
}
void update() {
    Json out;
    {
        std::lock_guard lock(resultMutex);
        out = std::move(workerResult);
    }
    if (worker.joinable())
        worker.join();
    busy = false;
    if (out.contains("error")) {
        connected = false;
        SetWindowTextW(status, (L"Core: Disconnected | " + pc::wide(out["error"])).c_str());
        return;
    }
    for (const auto &key : {"status", "rows", "config"})
        if (!out.at(key).value("success", false)) {
            connected = false;
            SetWindowTextW(status, (L"Core: Disconnected | " +
                                    pc::wide(out.at(key).value("errorMessage", "Protocol error")))
                                       .c_str());
            return;
        }
    connected = true;
    auto st = out.at("status").at("data");
    SetWindowTextW(
        status,
        (L"Core: Running | PID " + std::to_wstring(st.at("pid").get<DWORD>()) + L" | " +
         std::to_wstring(st.at("intervalMs").get<int>()) + L" ms" +
         (st.at("configHealthy").get<bool>() ? L"" : L" | CONFIG ERROR: repair config.json and reload"))
            .c_str());
    const Json newRows = out.at("rows").at("data");
    if (sameStructure(rows, newRows)) {
        updateRowsInPlace(newRows);
    } else {
        int oldTopIndex = ListView_GetTopIndex(list);
        int oldTopOffset = 0;
        std::optional<PresentationIdentity> oldTopProcess;
        RECT oldTopRect{};
        if (oldTopIndex >= 0 && oldTopIndex < static_cast<int>(rows.size())) {
            oldTopProcess = presentationIdentity(rows[oldTopIndex]);
            if (ListView_GetItemRect(list, oldTopIndex, &oldTopRect, LVIR_BOUNDS))
                oldTopOffset = oldTopRect.top;
        }

        rendering = true;
        rows = newRows;
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        int index = 0;
        for (const auto &process : rows) {
            auto values = cells(process);
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = index;
            item.pszText = values[0].data();
            ListView_InsertItem(list, &item);
            for (int column = 1; column < static_cast<int>(values.size()); ++column)
                ListView_SetItemText(list, index, column, values[column].data());
            ++index;
        }

        int selectedIndex = selectedProcess ? findProcess(*selectedProcess) : -1;
        if (selectedIndex >= 0) {
            ListView_SetItemState(list, selectedIndex, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetSelectionMark(list, selectedIndex);
        } else if (selectedProcess) {
            selectedProcess.reset();
            SetWindowTextW(detail, L"The selected process has ended.");
        }
        if (oldTopProcess)
            restoreTopProcess(*oldTopProcess, oldTopIndex, oldTopOffset);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
        rendering = false;
    }
    configuration = out.at("config").at("data");
    std::wstring r = L"Persistent rules (exact executable name)\r\n";
    for (const auto &key : {"priorityRules", "affinityRules"})
        for (const auto &rule : configuration.at(key))
            r += pc::wide(rule.at("name")) + L"  / " + pc::wide(key) + L"  / " +
                 pc::wide(rule.at("value").dump()) + L"\r\n";
    SetWindowTextW(rules, r.c_str());
    if (out.contains("action")) {
        auto a = out.at("action");
        if (!a.value("success", false))
            MessageBoxW(
                window,
                pc::wide("Win32 " + a.at("errorCode").dump() + ": " + a.at("errorMessage").get<std::string>())
                    .c_str(),
                L"PControl — operation failed", MB_OK | MB_ICONERROR);
        else
            SetWindowTextW(detail,
                           L"Operation succeeded. The process list updates on the next Core snapshot.");
    }
}
void command(int id) {
    if (id == AllCpu) {
        for (int i = 0; i < ListView_GetItemCount(cpus); ++i)
            ListView_SetCheckState(cpus, i, TRUE);
        return;
    }
    if (busy)
        return;
    if (id == Reconnect) {
        SetWindowTextW(status, L"Core: Connecting...");
        begin({}, true);
        return;
    }
    if (id == Reload) {
        begin({{"command", "ReloadConfiguration"}});
        return;
    }
    if (!connected)
        return;
    try {
        bool pr = id == ApplyPriority || id == SavePriority || id == RemovePriority;
        bool remove = id == RemovePriority || id == RemoveAffinity;
        bool save = id == SavePriority || id == SaveAffinity;
        bool current = id == ApplyPriority || id == ApplyAffinity;
        if (!remove && !save && !current)
            return;
        Json q;
        if (current) {
            auto p = selected();
            q = {{"pid", p.at("pid")},
                 {"creationTime", p.at("creationTime")},
                 {"command", pr ? "SetPriority" : "SetAffinity"}};
        } else {
            auto name = pc::utf8(text(ruleName));
            if (name.empty())
                throw pc::Error(ERROR_INVALID_PARAMETER, "Enter an executable name");
            q = {{"name", name},
                 {"command", remove ? (pr ? "RemovePersistentPriorityRule" : "RemovePersistentAffinityRule")
                                    : (pr ? "AddPersistentPriorityRule" : "AddPersistentAffinityRule")}};
        }
        if (!remove) {
            if (pr) {
                int i = (int)SendMessageW(priority, CB_GETCURSEL, 0, 0);
                if (i < 0)
                    return;
                if (i == 5 && MessageBoxW(window,
                                          L"Real Time priority can make Windows unresponsive and starve "
                                          L"essential system work. Apply this priority?",
                                          L"Real Time priority warning",
                                          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                    return;
                q["value"] = priorityValues[i];
            } else {
                Json chosen = Json::array();
                for (int i = 0; i < ListView_GetItemCount(cpus); ++i)
                    if (ListView_GetCheckState(cpus, i))
                        chosen.push_back(i);
                if (chosen.empty())
                    throw pc::Error(ERROR_INVALID_PARAMETER, "Select at least one CPU");
                q["value"] = {{"group", 0}, {"cpus", chosen}};
            }
        }
        begin(q);
    } catch (const std::exception &e) {
        MessageBoxW(window, pc::wide(e.what()).c_str(), L"PControl", MB_OK | MB_ICONWARNING);
    }
}
void layout() {
    RECT r;
    GetClientRect(window, &r);
    int width = r.right, tableHeight = std::max(180, (int)r.bottom - 350);
    MoveWindow(status, 12, 8, width - 250, 26, TRUE);
    MoveWindow(GetDlgItem(window, Reconnect), width - 230, 6, 106, 28, TRUE);
    MoveWindow(GetDlgItem(window, Reload), width - 118, 6, 106, 28, TRUE);
    MoveWindow(list, 12, 40, width - 24, tableHeight, TRUE);
    int y = tableHeight + 48;
    MoveWindow(detail, 12, y, width - 24, 36, TRUE);
    y += 40;
    MoveWindow(ruleName, 12, y, 230, 26, TRUE);
    MoveWindow(priority, 254, y, 150, 220, TRUE);
    MoveWindow(GetDlgItem(window, ApplyPriority), 416, y, 130, 28, TRUE);
    MoveWindow(GetDlgItem(window, SavePriority), 554, y, 130, 28, TRUE);
    MoveWindow(GetDlgItem(window, RemovePriority), 692, y, 145, 28, TRUE);
    y += 36;
    MoveWindow(cpus, 12, y, 392, 142, TRUE);
    MoveWindow(GetDlgItem(window, AllCpu), 416, y, 130, 28, TRUE);
    MoveWindow(GetDlgItem(window, ApplyAffinity), 416, y + 36, 130, 28, TRUE);
    MoveWindow(GetDlgItem(window, SaveAffinity), 416, y + 72, 130, 28, TRUE);
    MoveWindow(GetDlgItem(window, RemoveAffinity), 416, y + 108, 130, 28, TRUE);
    MoveWindow(rules, 554, y, width - 566, 142, TRUE);
}
LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        window = h;
        font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        status = control(L"STATIC", L"Core: Connecting...", 0, 0);
        control(L"BUTTON", L"Connect / start", BS_PUSHBUTTON, Reconnect);
        control(L"BUTTON", L"Reload config", BS_PUSHBUTTON, Reload);
        list = control(WC_LISTVIEWW, L"",
                       LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, Processes);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t *labels[] = {L"Process",      L"PID",    L"Priority",       L"Affinity mask",
                                   L"Architecture", L"Access", L"Executable path"};
        int widths[] = {200, 70, 110, 115, 100, 110, 450};
        for (int i = 0; i < 7; ++i) {
            LVCOLUMNW c{};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = (LPWSTR)labels[i];
            c.cx = widths[i];
            ListView_InsertColumn(list, i, &c);
        }
        detail = control(L"STATIC",
                         L"Select a process. Rules can also target an executable name typed below.", 0, 0);
        ruleName = control(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, RuleName);
        SendMessageW(ruleName, EM_SETCUEBANNER, TRUE, (LPARAM)L"Rule executable name, e.g. game.exe");
        priority = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, Priority);
        for (auto s : priorityNames)
            SendMessageW(priority, CB_ADDSTRING, 0, (LPARAM)s);
        SendMessageW(priority, CB_SETCURSEL, 2, 0);
        control(L"BUTTON", L"Apply priority", BS_PUSHBUTTON, ApplyPriority);
        control(L"BUTTON", L"Save priority rule", BS_PUSHBUTTON, SavePriority);
        control(L"BUTTON", L"Remove priority rule", BS_PUSHBUTTON, RemovePriority);
        cpus = control(WC_LISTVIEWW, L"", LVS_LIST | WS_BORDER | WS_TABSTOP, 0);
        ListView_SetExtendedListViewStyle(cpus, LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);
        DWORD count = std::min(GetActiveProcessorCount(0), DWORD(64));
        for (DWORD i = 0; i < count; ++i) {
            auto label = L"CPU " + std::to_wstring(i);
            LVITEMW it{};
            it.mask = LVIF_TEXT;
            it.iItem = (int)i;
            it.pszText = label.data();
            ListView_InsertItem(cpus, &it);
            ListView_SetCheckState(cpus, i, TRUE);
        }
        control(L"BUTTON", L"Select all CPUs", BS_PUSHBUTTON, AllCpu);
        control(L"BUTTON", L"Apply affinity", BS_PUSHBUTTON, ApplyAffinity);
        control(L"BUTTON", L"Save affinity rule", BS_PUSHBUTTON, SaveAffinity);
        control(L"BUTTON", L"Remove affinity", BS_PUSHBUTTON, RemoveAffinity);
        rules = control(L"EDIT", L"", ES_READONLY | ES_MULTILINE | WS_VSCROLL | WS_BORDER, 0);
        layout();
        SetTimer(h, 1, 1500, nullptr);
        begin({}, true);
        return 0;
    }
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)l)->ptMinTrackSize = {920, 640};
        return 0;
    case WM_TIMER:
        begin();
        return 0;
    case ready:
        try {
            update();
        } catch (const std::exception &e) {
            busy = false;
            rendering = false;
            connected = false;
            SetWindowTextW(status, (L"Core: Disconnected | Invalid response: " + pc::wide(e.what())).c_str());
        }
        return 0;
    case WM_COMMAND:
        command(LOWORD(w));
        return 0;
    case WM_NOTIFY:
        if (((NMHDR *)l)->hwndFrom == list && ((NMHDR *)l)->code == LVN_ITEMCHANGED && !rendering)
            selection();
        return 0;
    case WM_CLOSE:
        closing = true;
        KillTimer(h, 1);
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    InitCommonControls();
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpfnWndProc = proc;
    cls.lpszClassName = L"PControl.Window";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&cls);
    auto h = CreateWindowExW(0, cls.lpszClassName, L"PControl", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                             CW_USEDEFAULT, 1200, 780, nullptr, nullptr, instance, nullptr);
    if (!h)
        return 1;
    ShowWindow(h, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(h, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (worker.joinable())
        worker.join();
    return 0;
}
