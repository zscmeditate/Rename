// ============================================================================
// 视频文件批量重命名工具（原生 Win32 / C++）
//
// 功能：选择目录后，把该目录下所有 .mp4 / .mkv 文件（不递归、扩展名不区分
//       大小写）按文件名（不含扩展名）以与资源管理器一致的顺序排序，依次
//       重命名为 01.mp4、02.mkv …… 保留原扩展名；
//       文件总数超过 99 时统一改用三位编号（001、002……）。
//       采用"两阶段改名"（先统一改为唯一临时名，再改为目标名），避免批量
//       改名过程中的名字冲突、覆盖或文件丢失。
//
// 构建（MinGW-w64，静态链接，只依赖 Windows 系统 DLL）：
//   g++ main.cpp -o VideoRenamer.exe -municode -mwindows -O2 -s ^
//       -static -static-libgcc -static-libstdc++ ^
//       -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE ^
//       -lole32 -luuid -lshell32 -lshlwapi -lcomctl32 -lgdi32 -luser32
// ============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <cwctype>
#include <cstdio>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>
#include <cwchar>

// 控件 ID
enum {
    IDC_EDIT_PATH   = 1001,
    IDC_BTN_BROWSE  = 1002,
    IDC_BTN_RENAME  = 1003,
    IDC_EDIT_STATUS = 1004,
    IDC_BTN_UNDO    = 1005
};

static HINSTANCE g_hInst = nullptr;
static HWND g_hLabel = nullptr;
static HWND g_hPathEdit = nullptr;
static HWND g_hBrowseBtn = nullptr;
static HWND g_hRenameBtn = nullptr;
static HWND g_hUndoBtn = nullptr;
static HWND g_hStatusEdit = nullptr;
static HFONT g_hFont = nullptr;
static int g_dpi = 96;

// 最近一次重命名的记录，用于撤销：<当前文件名, 原文件名>
static std::wstring g_undoDir;
static std::vector<std::pair<std::wstring, std::wstring>> g_undo;

// 按当前 DPI 缩放一个 96 DPI 设计值
static int S(int v) { return MulDiv(v, g_dpi, 96); }

// 格式化 Win32 错误码为可读文本
static std::wstring FormatWin32Error(DWORD err)
{
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s;
    if (n && buf) {
        s.assign(buf, n);
        while (!s.empty() &&
               (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' '))
            s.pop_back();
        LocalFree(buf);
    }
    if (s.empty()) {
        wchar_t tmp[64];
        swprintf(tmp, 64, L"错误码 %lu", (unsigned long)err);
        s = tmp;
    }
    return s;
}

// 超长路径加 \\?\ 前缀，避免 MAX_PATH 限制
static std::wstring LongPath(const std::wstring& p)
{
    if (p.size() < 248) return p;
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

static std::wstring JoinPath(const std::wstring& dir, const std::wstring& name)
{
    if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
        return dir + name;
    return dir + L"\\" + name;
}

// 去掉首尾空白及用户粘贴时可能带上的引号
static std::wstring Trim(const std::wstring& s)
{
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return L"";
    size_t e = s.find_last_not_of(L" \t\r\n");
    std::wstring r = s.substr(b, e - b + 1);
    if (r.size() >= 2 && r.front() == L'"' && r.back() == L'"')
        r = r.substr(1, r.size() - 2);
    return r;
}

static bool PathExists(const std::wstring& p)
{
    return GetFileAttributesW(LongPath(p).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 生成目录内一个当前不存在的临时文件名（重命名两个阶段都用它避免互相覆盖）
static std::wstring NewTempPath(const std::wstring& dir, DWORD seed, int& seq)
{
    for (int k = 0; k < 1000; ++k) {
        wchar_t tmp[160];
        swprintf(tmp, 160, L".vrn_tmp_%lu_%d_%d.tmp",
                 (unsigned long)seed, seq++, k);
        std::wstring candidate = JoinPath(dir, tmp);
        if (!PathExists(candidate)) return candidate;
    }
    return L"";
}

// 按"文件名（不含扩展名）"排序：使用系统 StrCmpLogicalW，和资源管理器一致，
// 数字按数值比较（我的视频 2 排在 我的视频 10 前面）
static int CompareBaseName(const std::wstring& nameA, const std::wstring& extA,
                           const std::wstring& nameB, const std::wstring& extB)
{
    std::wstring baseA = nameA.substr(0, nameA.size() - extA.size());
    std::wstring baseB = nameB.substr(0, nameB.size() - extB.size());
    int r = StrCmpLogicalW(baseA.c_str(), baseB.c_str());
    if (r != 0) return r;
    // 主名相同时按扩展名决定先后，保证顺序稳定
    return StrCmpLogicalW(extA.c_str(), extB.c_str());
}

static void SetStatus(const std::wstring& text)
{
    if (g_hStatusEdit) SetWindowTextW(g_hStatusEdit, text.c_str());
}

// 打开系统文件夹选择对话框
static void OnBrowse(HWND hwnd)
{
    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
    if (FAILED(hr) || !dlg) {
        SetStatus(L"无法打开系统文件夹选择对话框：" + FormatWin32Error((DWORD)hr));
        return;
    }

    FILEOPENDIALOGOPTIONS opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts)))
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                        FOS_PATHMUSTEXIST);

    // 若输入框里已有合法目录，则从该目录开始浏览
    wchar_t cur[32768] = {0};
    GetWindowTextW(g_hPathEdit, cur, 32768);
    std::wstring startDir = Trim(cur);
    if (!startDir.empty() && PathExists(startDir)) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(startDir.c_str(), nullptr,
                                                   IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }

    if (SUCCEEDED(dlg->Show(hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                SetWindowTextW(g_hPathEdit, path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
}

// 核心：批量重命名
static void DoRename(HWND hwnd)
{
    wchar_t rawBuf[32768] = {0};
    GetWindowTextW(g_hPathEdit, rawBuf, 32768);
    std::wstring raw = Trim(rawBuf);
    if (raw.empty()) {
        SetStatus(L"请先选择或输入要处理的文件夹路径。");
        SetFocus(g_hPathEdit);
        return;
    }

    // 规范化为绝对路径
    wchar_t full[32768] = {0};
    DWORD n = GetFullPathNameW(raw.c_str(), 32768, full, nullptr);
    std::wstring dir = (n > 0 && n < 32768) ? std::wstring(full) : raw;
    while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/'))
        dir.pop_back();

    DWORD attr = GetFileAttributesW(LongPath(dir).c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        SetStatus(L"文件夹不存在或无法访问：\r\n" + dir + L"\r\n\r\n原因：" +
                  FormatWin32Error(GetLastError()));
        return;
    }
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        SetStatus(L"该路径不是文件夹：\r\n" + dir);
        return;
    }
    SetWindowTextW(g_hPathEdit, dir.c_str());

    struct Item {
        std::wstring name;  // 原文件名
        std::wstring ext;   // 原扩展名（保留原始大小写）
        std::wstring temp;  // 第一阶段临时名
    };

    // 1. 收集当前目录下的 .mp4 / .mkv（不递归）
    std::vector<Item> items;
    WIN32_FIND_DATAW fd;
    std::wstring pattern = JoinPath(dir, L"*");
    HANDLE hFind = FindFirstFileW(LongPath(pattern).c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        SetStatus(L"无法读取文件夹内容：\r\n" + dir + L"\r\n\r\n原因：" +
                  FormatWin32Error(GetLastError()));
        return;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring::npos || dot == 0) continue;
        std::wstring ext = name.substr(dot);
        std::wstring low = ext;
        std::transform(low.begin(), low.end(), low.begin(),
                       [](wchar_t c) { return (wchar_t)towlower(c); });
        if (low != L".mp4" && low != L".mkv") continue;
        Item it;
        it.name = name;
        it.ext = ext;
        items.push_back(it);
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    if (items.empty()) {
        SetStatus(L"该文件夹中未找到 .mp4 或 .mkv 文件。\r\n\r\n" + dir);
        return;
    }

    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) {
                  return CompareBaseName(a.name, a.ext, b.name, b.ext) < 0;
              });

    // 2. 第一阶段：全部改为互不冲突的临时名，彻底避免互相覆盖
    std::vector<Item> staged;
    std::vector<std::wstring> errors;
    DWORD seed = GetTickCount();
    int seq = 0;

    for (size_t i = 0; i < items.size(); ++i) {
        std::wstring temp = NewTempPath(dir, seed, seq);
        if (temp.empty()) {
            errors.push_back(L"【" + items[i].name +
                             L"】无法生成可用的临时文件名（第一步未执行）");
            continue;
        }
        if (!MoveFileExW(LongPath(JoinPath(dir, items[i].name)).c_str(),
                         LongPath(temp).c_str(), 0)) {
            errors.push_back(L"【" + items[i].name + L"】改名失败（第一步）：" +
                             FormatWin32Error(GetLastError()));
            continue;
        }
        items[i].temp = temp;
        staged.push_back(items[i]);
    }

    // 3. 第二阶段：临时名改为编号 + 原扩展名
    //    文件总数不超过 99 时用两位（01、02……）；超过 99 时统一用三位（001、002……）
    int width = (items.size() > 99) ? 3 : 2;
    int success = 0;
    bool anyChanged = false;
    std::vector<std::pair<std::wstring, std::wstring>> undo;
    for (size_t i = 0; i < staged.size(); ++i) {
        Item& it = staged[i];
        wchar_t base[32];
        swprintf(base, 32, L"%0*d", width, (int)i + 1);
        std::wstring newName = std::wstring(base) + it.ext;
        std::wstring target = JoinPath(dir, newName);

        if (MoveFileExW(LongPath(it.temp).c_str(), LongPath(target).c_str(), 0)) {
            ++success;
            if (newName != it.name) anyChanged = true;
            undo.push_back(std::make_pair(newName, it.name));
            continue;
        }

        DWORD err = GetLastError();
        std::wstring reason = FormatWin32Error(err);
        // 第二阶段失败：尽量把文件恢复成原名，避免用户看到陌生临时名
        if (MoveFileExW(LongPath(it.temp).c_str(),
                        LongPath(JoinPath(dir, it.name)).c_str(), 0)) {
            errors.push_back(L"【" + it.name + L"】改名失败（第二步）：" + reason +
                             L"（已恢复原文件名）");
        } else {
            errors.push_back(L"【" + it.name + L"】改名失败（第二步）：" + reason +
                             L"；且恢复原文件名失败，该文件当前名为：" + it.temp);
        }
    }

    // 4. 汇总结果
    std::wstring msg;
    msg += L"处理完成：成功重命名 " + std::to_wstring(success) + L" 个文件";
    if (!errors.empty())
        msg += L"，失败 " + std::to_wstring(errors.size()) + L" 个";
    msg += L"。\r\n";
    msg += L"共找到 " + std::to_wstring(items.size()) + L" 个视频文件，目录：\r\n";
    msg += dir + L"\r\n";
    if (success > 0) {
        msg += L"\r\n重命名明细（原文件名 → 新文件名）：\r\n";
        for (const auto& p : undo)
            msg += L"· " + p.second + L" → " + p.first + L"\r\n";
    }
    if (anyChanged || !g_undo.empty())
        msg += L"\r\n如需恢复原文件名，可点击「撤销重命名」。\r\n";
    if (!errors.empty()) {
        msg += L"\r\n失败详情：\r\n";
        for (const auto& e : errors) msg += L"· " + e + L"\r\n";
    }
    SetStatus(msg);

    // 记录本次成功项供撤销使用（本次文件名无实际变化或全部失败时保留上一次记录）
    if (success > 0 && anyChanged) {
        g_undoDir = dir;
        g_undo.swap(undo);
        if (g_hUndoBtn) EnableWindow(g_hUndoBtn, TRUE);
    } else if (success > 0 && g_undo.empty()) {
        // 全部文件名本来就是目标名：无可撤销内容
        if (g_hUndoBtn) EnableWindow(g_hUndoBtn, FALSE);
    }
}

// 撤销最近一次重命名：把 01、02…… 改回原文件名
static void DoUndo(HWND hwnd)
{
    if (g_undo.empty()) {
        SetStatus(L"没有可撤销的重命名操作。");
        return;
    }

    const std::wstring dir = g_undoDir;
    DWORD attr = GetFileAttributesW(LongPath(dir).c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        SetStatus(L"原文件夹已不存在，无法撤销：\r\n" + dir);
        return;
    }

    struct UItem {
        std::wstring cur;   // 当前（编号后）文件名
        std::wstring orig;  // 原文件名
        std::wstring temp;  // 临时名
    };

    std::vector<std::wstring> errors;
    std::vector<std::wstring> skipped;
    std::vector<UItem> list;
    for (const auto& p : g_undo) {
        UItem u;
        u.cur = p.first;
        u.orig = p.second;
        if (!PathExists(JoinPath(dir, u.cur))) {
            skipped.push_back(u.cur);
            continue;
        }
        list.push_back(u);
    }

    if (list.empty()) {
        std::wstring msg = L"撤销未执行：原文件都不在原文件夹中。\r\n目录：\r\n" + dir;
        if (!skipped.empty()) {
            msg += L"\r\n已不存在的文件：\r\n";
            for (const auto& e : skipped) msg += L"· " + e + L"\r\n";
        }
        SetStatus(msg);
        return;
    }

    // 第一步：先改成互不冲突的临时名，避免恢复过程中互相覆盖
    DWORD seed = GetTickCount();
    int seq = 0;
    std::vector<UItem> staged;
    for (auto& u : list) {
        std::wstring temp = NewTempPath(dir, seed, seq);
        if (temp.empty() ||
            !MoveFileExW(LongPath(JoinPath(dir, u.cur)).c_str(),
                         LongPath(temp).c_str(), 0)) {
            errors.push_back(L"【" + u.cur + L"】撤销失败（第一步）：" +
                             FormatWin32Error(GetLastError()));
            continue;
        }
        u.temp = temp;
        staged.push_back(u);
    }

    // 第二步：临时名改回原文件名
    int restored = 0;
    std::vector<std::pair<std::wstring, std::wstring>> done;
    for (auto& u : staged) {
        std::wstring target = JoinPath(dir, u.orig);
        if (MoveFileExW(LongPath(u.temp).c_str(), LongPath(target).c_str(), 0)) {
            ++restored;
            done.push_back(std::make_pair(u.cur, u.orig));
            continue;
        }
        DWORD err = GetLastError();
        std::wstring reason = FormatWin32Error(err);
        if (MoveFileExW(LongPath(u.temp).c_str(),
                        LongPath(JoinPath(dir, u.cur)).c_str(), 0)) {
            errors.push_back(L"【" + u.cur + L"】撤销失败（第二步）：" + reason +
                             L"（已保留编号文件名）");
        } else {
            errors.push_back(L"【" + u.cur + L"】撤销失败（第二步）：" + reason +
                             L"；且恢复编号文件名失败，该文件当前名为：" + u.temp);
        }
    }

    std::wstring msg = L"撤销完成：已恢复 " + std::to_wstring(restored) + L" 个文件的原名";
    if (!errors.empty())
        msg += L"，失败 " + std::to_wstring(errors.size()) + L" 个";
    msg += L"。\r\n目录：\r\n" + dir + L"\r\n";
    if (!done.empty()) {
        msg += L"\r\n恢复明细（编号名 → 原文件名）：\r\n";
        for (const auto& p : done)
            msg += L"· " + p.first + L" → " + p.second + L"\r\n";
    }
    if (!skipped.empty())
        msg += L"\r\n另有 " + std::to_wstring(skipped.size()) +
               L" 个文件已不存在，已跳过。\r\n";
    if (!errors.empty()) {
        msg += L"\r\n详情：\r\n";
        for (const auto& e : errors) msg += L"· " + e + L"\r\n";
        msg += L"\r\n仍有文件保持编号名称，可再次尝试撤销。\r\n";
    }
    SetStatus(msg);

    // 全部恢复成功则清除记录并禁用按钮；否则保留记录便于重试
    if (errors.empty()) {
        g_undo.clear();
        g_undoDir.clear();
        if (g_hUndoBtn) EnableWindow(g_hUndoBtn, FALSE);
    }
}

static void LayoutChildren(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int m = S(16);

    int labelW = S(76);
    int btnW = S(92);
    int editH = S(26);

    MoveWindow(g_hLabel, m, m + S(3), labelW, S(22), TRUE);

    int editX = m + labelW + S(6);
    int editW = W - editX - m - btnW - S(8);
    MoveWindow(g_hPathEdit, editX, m, editW, editH, TRUE);
    MoveWindow(g_hBrowseBtn, W - m - btnW, m + S(1), btnW, S(28), TRUE);

    int renameY = m + editH + S(14);
    MoveWindow(g_hRenameBtn, m, renameY, S(120), S(32), TRUE);
    MoveWindow(g_hUndoBtn, m + S(120) + S(10), renameY, S(120), S(32), TRUE);

    int statusY = renameY + S(32) + S(14);
    MoveWindow(g_hStatusEdit, m, statusY, W - 2 * m, H - statusY - m, TRUE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        g_hFont = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL,
                              FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE,
                              L"Microsoft YaHei UI");
        if (!g_hFont)
            g_hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        g_hLabel = CreateWindowExW(
            0, L"STATIC", L"视频目录：",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, hwnd, nullptr, g_hInst, nullptr);

        g_hPathEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_EDIT_PATH, g_hInst, nullptr);

        g_hBrowseBtn = CreateWindowExW(
            0, L"BUTTON", L"选择目录…",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_BROWSE, g_hInst, nullptr);

        g_hRenameBtn = CreateWindowExW(
            0, L"BUTTON", L"重命名",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_RENAME, g_hInst, nullptr);

        g_hStatusEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE |
                ES_AUTOVSCROLL | ES_READONLY,
            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_EDIT_STATUS, g_hInst, nullptr);

        g_hUndoBtn = CreateWindowExW(
            0, L"BUTTON", L"撤销重命名",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_UNDO, g_hInst, nullptr);
        EnableWindow(g_hUndoBtn, FALSE);

        HWND all[] = {g_hLabel, g_hPathEdit, g_hBrowseBtn,
                      g_hRenameBtn, g_hUndoBtn, g_hStatusEdit};
        for (HWND c : all) SendMessageW(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        SetStatus(L"请选择或输入要处理的文件夹，然后点击「重命名」。\r\n\r\n"
                  L"规则：当前文件夹（不含子文件夹）中的 .mp4 / .mkv 文件，"
                  L"按文件名（不含扩展名）以与资源管理器一致的顺序排序，"
                  L"依次重命名为 01、02、03……并保留各自原扩展名；"
                  L"文件超过 99 个时统一改用三位编号 001、002、003……\r\n"
                  L"重命名后可点击「撤销重命名」恢复原文件名。");

        LayoutChildren(hwnd);
        SetFocus(g_hPathEdit);
        return 0;
    }

    case WM_SIZE:
        if (g_hStatusEdit) LayoutChildren(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_BROWSE:
            OnBrowse(hwnd);
            return 0;
        case IDC_BTN_RENAME:
            DoRename(hwnd);
            return 0;
        case IDC_BTN_UNDO:
            DoUndo(hwnd);
            return 0;
        }
        break;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = S(520);
        mmi->ptMinTrackSize.y = S(320);
        return 0;
    }

    case WM_DESTROY:
        if (g_hFont) DeleteObject(g_hFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    g_hInst = hInstance;

    // 高分屏适配
    SetProcessDPIAware();
    HDC screenDC = GetDC(nullptr);
    if (screenDC) {
        g_dpi = GetDeviceCaps(screenDC, LOGPIXELSX);
        ReleaseDC(nullptr, screenDC);
    }
    if (g_dpi <= 0) g_dpi = 96;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"VideoRenamerWindow";
    if (!RegisterClassExW(&wc)) return 0;

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
                  WS_THICKFRAME | WS_MAXIMIZEBOX;
    RECT wr = {0, 0, S(660), S(360)};
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    int ww = wr.right - wr.left;
    int wh = wr.bottom - wr.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"视频批量重命名工具",
        style, x, y, ww, wh,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow == SW_SHOWDEFAULT ? SW_SHOWNORMAL : nCmdShow);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    CoUninitialize();
    return (int)m.wParam;
}
