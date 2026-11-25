#pragma once

// The window a GDKX title shows its frames in, for PresentX.
//
// On the console PresentX puts a frame on the display: it names no window. A GDKX title still makes its own top-level
// Win32 window (CreateWindowExW with its own class, a message loop on its own thread) before it presents, so on the PC
// that window is where the frames belong. Measured on a retail executable: by its first PresentX the title has one
// visible, unowned top-level window on a thread of its own; the other windows of the process at that point are hidden
// (the IME windows Windows adds, a driver's dummy window).
//
// The lookup sends no message to any window. The present thread is not the title's window thread, and that thread can
// be busy (or waiting on the present thread) when the first PresentX comes: GetWindowTextW, or any call that sends a
// message across threads, would block the present thread there. Measured: a lookup that read the window titles with
// GetWindowTextW stopped the retail title at its first PresentX. EnumWindows, GetWindowThreadProcessId, IsWindowVisible,
// GetWindow(GW_OWNER), GetWindowLongPtrW and GetClientRect read the window manager's own state and send nothing.

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rt::d3d12 {

// Computes a centered 16:9 outer window rectangle (x, y, w, h) that fits cleanly inside the
// monitor's work area (~88% cap, client dimensions multiples of 16) for the given window styles.
inline bool compute_fitted_window_rect(HWND reference_window, DWORD style, DWORD ex_style, int& out_x, int& out_y,
                                       int& out_w, int& out_h, int* out_client_w = nullptr,
                                       int* out_client_h = nullptr) {
    RECT work{};
    HMONITOR mon = reference_window ? MonitorFromWindow(reference_window, MONITOR_DEFAULTTOPRIMARY)
                                    : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        work = mi.rcWork;
    } else if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work = RECT{0, 0, 1280, 720};
    }
    const int work_w = (std::max)(320, int(work.right - work.left));
    const int work_h = (std::max)(240, int(work.bottom - work.top));

    RECT probe{0, 0, 1280, 720};
    AdjustWindowRectEx(&probe, style, FALSE, ex_style);
    const int chrome_w = (probe.right - probe.left) - 1280;
    const int chrome_h = (probe.bottom - probe.top) - 720;

    const int max_outer_w = (work_w * 88) / 100;
    const int max_outer_h = (work_h * 88) / 100;
    const int avail_cw = (std::max)(320, max_outer_w - chrome_w);
    const int avail_ch = (std::max)(180, max_outer_h - chrome_h);

    // Pick largest 16:9 client area (16*k x 9*k) fitting inside (avail_cw, avail_ch), with k even so w is a multiple of 32.
    int k = (std::min)(avail_cw / 16, avail_ch / 9);
    if (k > 2) k &= ~1;
    const int cw = (std::max)(320, k * 16);
    const int ch = (std::max)(180, k * 9);

    RECT outer{0, 0, cw, ch};
    AdjustWindowRectEx(&outer, style, FALSE, ex_style);
    out_w = (std::min)(work_w, int(outer.right - outer.left));
    out_h = (std::min)(work_h, int(outer.bottom - outer.top));
    out_x = work.left + (work_w - out_w) / 2;
    out_y = work.top + (work_h - out_h) / 2;
    if (out_client_w) *out_client_w = cw;
    if (out_client_h) *out_client_h = ch;
    return true;
}

// Fits an oversized top-level title window (e.g., 3840x2160 on a 1080p display) into the monitor work area.
// Must be called on the window's own thread (e.g., from ShowWindow shim) so SetWindowPos never blocks across threads.
inline void fit_title_window_to_monitor(HWND window) {
    if (!window) return;
    char opt[8]{};
    if (GetEnvironmentVariableA("LEONIDA_FIT_WINDOW", opt, sizeof(opt)) == 1 && opt[0] == '0') return;
    if (GetWindow(window, GW_OWNER)) return;
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    if ((style & WS_CHILD) || (ex_style & WS_EX_TOOLWINDOW)) return;

    RECT wr{};
    if (!GetWindowRect(window, &wr)) return;
    const int cur_w = wr.right - wr.left;
    const int cur_h = wr.bottom - wr.top;
    if (cur_w <= 0 || cur_h <= 0) return;

    RECT work{};
    HMONITOR mon = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        work = mi.rcWork;
    } else if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        return;
    }
    const int work_w = work.right - work.left;
    const int work_h = work.bottom - work.top;
    if (cur_w <= work_w && cur_h <= work_h) return;

    int x = 0, y = 0, w = 0, h = 0, cw = 0, ch = 0;
    if (!compute_fitted_window_rect(window, style, ex_style, x, y, w, h, &cw, &ch)) return;
    SetWindowPos(window, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    std::fprintf(stderr, "[rt] window: resized oversized title window %dx%d -> %dx%d (client %dx%d, work %dx%d)\n",
                 cur_w, cur_h, w, h, cw, ch, work_w, work_h);
}

// The visible, unowned, non-tool top-level window of `process_id` with the largest client area, `exclude` aside (the
// runtime's own fallback window); null when the process has none. Ties keep the first one in Z order (the topmost).
inline HWND find_title_window(DWORD process_id, HWND exclude = nullptr) {
    struct Search {
        DWORD process_id;
        HWND exclude;
        HWND best;
        LONGLONG best_area;
    } search{process_id, exclude, nullptr, -1};
    EnumWindows(
        [](HWND window, LPARAM context) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(context);
            DWORD owner_process = 0;
            if (window == s.exclude || !GetWindowThreadProcessId(window, &owner_process) ||
                owner_process != s.process_id)
                return TRUE;
            if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
            if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
            RECT client{};
            const LONGLONG area =
                GetClientRect(window, &client) ? LONGLONG(client.right - client.left) * (client.bottom - client.top) : 0;
            if (area > s.best_area) {
                s.best = window;
                s.best_area = area;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.best;
}

}  // namespace rt::d3d12
