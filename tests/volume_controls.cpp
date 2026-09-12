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
    waveOutClose(g_wave_out);
    g_wave_out = nullptr;

    HWND controls[] = {g_pause_button, g_prev_frame_button, g_next_frame_button,
        g_split_button, g_dlss_button, g_model_button, g_mute_button,
        g_volume_label, g_volume_slider, g_trackbar};
    for (int width : {320, 640, 960}) {
        SetWindowPos(g_hwnd, nullptr, 0, 0, width, 300, SWP_NOMOVE | SWP_NOZORDER);
        LayoutControls(g_hwnd);
        RECT client; GetClientRect(g_hwnd, &client);
        for (int i = 0; i < 10; ++i) {
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
    DestroyWindow(g_hwnd);
    puts("PASS: slider/mute dispatch, stereo output levels, retained settings, independent seek, responsive layout");
}
