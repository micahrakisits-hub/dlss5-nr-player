#define wmain player_main
#include "../nr_player.cpp"
#undef wmain
#include <cassert>

int main()
{
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    assert(SetupWindow(960, 540));
    ShowWindow(g_hwnd, SW_HIDE);
    assert(g_volume == 100 && !g_muted);
    SendMessageW(g_volume_slider, TBM_SETPOS, TRUE, 35);
    WndProc(g_hwnd, WM_HSCROLL, TB_THUMBTRACK, (LPARAM)g_volume_slider);
    assert(g_volume == 35 && !g_seek_requested);
    WndProc(g_hwnd, WM_COMMAND, MAKEWPARAM(0, BN_CLICKED), (LPARAM)g_mute_button);
    assert(g_muted && g_volume == 35);
    SetVolume(60);
    assert(g_muted && g_volume == 60);

    // An audio stream created after a seek or file change inherits the settings.
    WAVEFORMATEX wf = {0x0003, 2, 48000, 384000, 8, 32, 0};
    assert(waveOutOpen(&g_wave_out, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR);
    ApplyVolumeLocked();
    DWORD volume = ~0u;
    assert(waveOutGetVolume(g_wave_out, &volume) == MMSYSERR_NOERROR && volume == 0);
    ToggleMute();
    assert(!g_muted && g_volume == 60);
    assert(waveOutGetVolume(g_wave_out, &volume) == MMSYSERR_NOERROR);
    assert(LOWORD(volume) == 39321 && HIWORD(volume) == 39321);
    SetVolume(0);
    assert(waveOutGetVolume(g_wave_out, &volume) == MMSYSERR_NOERROR && volume == 0);
    SetVolume(100);
    assert(waveOutGetVolume(g_wave_out, &volume) == MMSYSERR_NOERROR && volume == 0xffffffff);
    wchar_t volumeText[32];
    GetWindowTextW(g_volume_label, volumeText, 32);
    assert(std::wcscmp(volumeText, L"Volume: 100%") == 0);
    HDC labelDC = GetDC(g_volume_label);
    HFONT labelFont = (HFONT)SendMessageW(g_volume_label, WM_GETFONT, 0, 0);
    HGDIOBJ oldFont = labelFont ? SelectObject(labelDC, labelFont) : nullptr;
    SIZE volumeTextSize = {};
    GetTextExtentPoint32W(labelDC, volumeText, (int)std::wcslen(volumeText), &volumeTextSize);
    if (oldFont) SelectObject(labelDC, oldFont);
    ReleaseDC(g_volume_label, labelDC);
    RECT labelRect; GetClientRect(g_volume_label, &labelRect);
    assert(volumeTextSize.cx <= labelRect.right - labelRect.left);
    waveOutClose(g_wave_out);
    g_wave_out = nullptr;

    g_media_loaded = true;
    g_paused = false;
    assert(GetWindowLongPtrW(g_video_hwnd, GWL_STYLE) & SS_NOTIFY);
    WndProc(g_hwnd, WM_COMMAND, MAKEWPARAM(0, STN_CLICKED), (LPARAM)g_video_hwnd);
    assert(g_paused);
    WndProc(g_hwnd, WM_COMMAND, MAKEWPARAM(0, STN_CLICKED), (LPARAM)g_video_hwnd);
    assert(!g_paused);
    g_media_loaded = false;

    HWND controls[] = {g_pause_button, g_prev_frame_button, g_next_frame_button,
        g_split_button, g_dlss_button, g_model_button, g_fullscreen_button,
        g_mute_button, g_volume_label, g_volume_slider, g_trackbar};
    RECT initial[11], restored[11];
    SetWindowPos(g_hwnd, nullptr, 0, 0, 960, 400, SWP_NOMOVE | SWP_NOZORDER);
    LayoutControls(g_hwnd);
    for (int i = 0; i < 11; ++i) GetWindowRect(controls[i], &initial[i]);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 320, 300, SWP_NOMOVE | SWP_NOZORDER);
    LayoutControls(g_hwnd);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 960, 400, SWP_NOMOVE | SWP_NOZORDER);
    LayoutControls(g_hwnd);
    for (int i = 0; i < 11; ++i) {
        GetWindowRect(controls[i], &restored[i]);
        assert(EqualRect(&initial[i], &restored[i]));
    }
    for (int width : {320, 640, 960}) {
        SetWindowPos(g_hwnd, nullptr, 0, 0, width, 300, SWP_NOMOVE | SWP_NOZORDER);
        LayoutControls(g_hwnd);
        RECT client; GetClientRect(g_hwnd, &client);
        for (int i = 0; i < 11; ++i) {
            RECT a; GetWindowRect(controls[i], &a);
            MapWindowPoints(nullptr, g_hwnd, (POINT*)&a, 2);
            assert(a.left >= 0 && a.right <= client.right && a.top >= 0 && a.bottom <= client.bottom);
            for (int j = 0; j < i; ++j) {
                RECT b, overlap; GetWindowRect(controls[j], &b);
                MapWindowPoints(nullptr, g_hwnd, (POINT*)&b, 2);
                assert(!IntersectRect(&overlap, &a, &b));
            }
        }
    }

    RECT fitted = FitVideoRect(1000, 600, 1920, 1080);
    assert(fitted.left == 0 && fitted.right == 1000);
    assert(fitted.top == 19 && fitted.bottom == 581);
    fitted = FitVideoRect(1000, 600, 3840, 1080);
    assert(fitted.left == 0 && fitted.right == 1000);
    assert(fitted.top == 159 && fitted.bottom == 440);
    fitted = FitVideoRect(1000, 600, 1080, 1920);
    assert(fitted.top == 0 && fitted.bottom == 600);
    assert(fitted.left == 331 && fitted.right == 668);

    g_media_loaded = true; g_vid_w = 1920; g_vid_h = 1080; g_side = false;
    SetWindowPos(g_hwnd, nullptr, 0, 0, 1000, 700, SWP_NOMOVE | SWP_NOZORDER);
    LayoutControls(g_hwnd);
    RECT player; GetWindowRect(g_video_hwnd, &player);
    assert(abs((player.right - player.left) * 9 - (player.bottom - player.top) * 16) <= 16);
    g_side = true;
    LayoutControls(g_hwnd);
    GetWindowRect(g_video_hwnd, &player);
    assert(abs((player.right - player.left) * 9 - (player.bottom - player.top) * 32) <= 32);
    g_side = false;
    DWORD windowedStyle = (DWORD)GetWindowLongPtrW(g_hwnd, GWL_STYLE);
    HMENU windowedMenu = GetMenu(g_hwnd);
    ToggleFullscreen();
    assert(g_fullscreen);
    assert(!(GetWindowLongPtrW(g_hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW));
    assert(GetMenu(g_hwnd) == nullptr);
    for (HWND control : controls) assert(!(GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE));
    WndProc(g_hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
    assert(!g_fullscreen);
    DWORD restoredStyle = (DWORD)GetWindowLongPtrW(g_hwnd, GWL_STYLE);
    assert((restoredStyle & WS_OVERLAPPEDWINDOW) == (windowedStyle & WS_OVERLAPPEDWINDOW));
    assert(GetMenu(g_hwnd) == windowedMenu);
    for (HWND control : controls) assert(GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE);
    DestroyWindow(g_hwnd);
    puts("PASS: playback controls, reversible layout, aspect-ratio fitting, and fullscreen restore");
}
