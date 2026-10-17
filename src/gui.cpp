/*------------------------------------------------------------------------------
  xpad-mouse GUI v2 — Xbox 手柄摇杆映射鼠标（图形界面版）
  基于 Gopher360 (https://github.com/Tylemagne/Gopher360, GPL-3.0) 改装。

  v2 美化：渐变、辉光（AlphaBlend 预渲染贴图）、LED 灯衰减、
          摇杆弹性跟随 + 拖尾、胶囊开关、呼吸状态灯、点阵预览背景。
  与控制台版 (main.cpp) 共用 config.ini 与核心算法。
-------------------------------------------------------------------------------*/
#include <windows.h>
#include <windowsx.h>
#include <xinput.h>
#include <dwmapi.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "CXBOXController.h"
#include "ConfigFile.h"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#define DWMWCP_ROUND 2
#endif

// ======================= 颜色主题 =======================
static const COLORREF C_BG        = RGB(14, 16, 20);
static const COLORREF C_BG_TOP    = RGB(17, 20, 27);
static const COLORREF C_CARD      = RGB(22, 26, 34);
static const COLORREF C_INSET     = RGB(13, 16, 23);
static const COLORREF C_BORDER    = RGB(38, 45, 58);
static const COLORREF C_BORDER_DK = RGB(28, 34, 46);
static const COLORREF C_TEXT      = RGB(232, 236, 244);
static const COLORREF C_DIM       = RGB(138, 147, 168);
static const COLORREF C_ACCENT    = RGB(56, 224, 139);
static const COLORREF C_ACCENT_DK = RGB(31, 168, 98);
static const COLORREF C_BLUE      = RGB(86, 194, 255);
static const COLORREF C_AMBER     = RGB(255, 180, 74);
static const COLORREF C_THUMB     = RGB(240, 244, 250);
static const COLORREF C_GRID      = RGB(26, 33, 48);

// ======================= 辉光贴图 =======================
struct GlowSprite
{
  HDC dc = nullptr;
  HBITMAP bmp = nullptr;
  int n = 0;
};

static GlowSprite g_glowGreen, g_glowBlue, g_glowWhite, g_glowAmber;

static GlowSprite makeGlow(COLORREF c, int size = 96)
{
  GlowSprite g;
  g.n = size;
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = size;
  bi.bmiHeader.biHeight = -size;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void *bits = nullptr;
  g.bmp = CreateDIBSection(nullptr, &bi, 0 /*DIB_RGB_VALUES*/, &bits, nullptr, 0);
  DWORD *px = (DWORD *)bits;
  float half = (size - 1) / 2.f;
  int R = GetRValue(c), G = GetGValue(c), B = GetBValue(c);
  for (int y = 0; y < size; y++)
  {
    for (int x = 0; x < size; x++)
    {
      float dx = x - half, dy = y - half;
      float d = sqrtf(dx * dx + dy * dy) / half;
      float a = 1.f - d;
      if (a < 0.f) a = 0.f;
      a = a * a * a;                    // 陡峭衰减，中心亮边缘柔
      int A = (int)(a * 255.f);
      px[y * size + x] = ((DWORD)A << 24) |
                         ((DWORD)(R * A / 255) << 16) |
                         ((DWORD)(G * A / 255) << 8) |
                         (DWORD)(B * A / 255);
    }
  }
  g.dc = CreateCompatibleDC(nullptr);
  SelectObject(g.dc, g.bmp);
  return g;
}

// 在 (cx,cy) 画半径 radius 的辉光，strength 0-255
static void drawGlow(HDC hdc, const GlowSprite &g, int cx, int cy, int radius, BYTE strength)
{
  if (!g.dc || radius <= 0) return;
  BLENDFUNCTION bf = { AC_SRC_OVER, 0, strength, AC_SRC_ALPHA };
  GdiAlphaBlend(hdc, cx - radius, cy - radius, radius * 2, radius * 2,
                g.dc, 0, 0, g.n, g.n, bf);
}

// ======================= 全局状态 =======================
static HWND g_hwnd = nullptr;
static HFONT g_fontTitle = nullptr;
static HFONT g_fontHead = nullptr;
static HFONT g_font = nullptr;
static HFONT g_fontSmall = nullptr;
static HFONT g_fontNum = nullptr;

static std::atomic<bool>  g_stop{false};
static std::atomic<bool>  g_enabled{true};
static std::atomic<bool>  g_pauseWhenFocused{true};
static std::atomic<bool>  g_connected{false};
static std::atomic<int>   g_padIndex{-1};
static std::atomic<int>   g_lx{0}, g_ly{0}, g_rx{0}, g_ry{0};
static std::atomic<unsigned short> g_buttons{0};
static std::atomic<int>   g_lt{0}, g_rt{0};      // 扳机原始值 0-255
static std::atomic<float> g_speed{0.03f};
static std::atomic<float> g_deadzone{6000.f};
static std::atomic<float> g_accel{0.f};
static std::atomic<bool>  g_swap{false};
static std::atomic<int>   g_speedPreset{2};
static std::atomic<DWORD> g_cfgMouseLeft{XINPUT_GAMEPAD_A};
static std::atomic<DWORD> g_cfgMouseRight{XINPUT_GAMEPAD_B};
static std::atomic<DWORD> g_cfgMouseMiddle{XINPUT_GAMEPAD_X};
static std::atomic<DWORD> g_cfgMouseX1{0};
static std::atomic<DWORD> g_cfgMouseX2{0};
static HANDLE g_thread = nullptr;

// 虚拟按键位：扳机并入 16 位以上的位域
static const DWORD VB_LT = 0x10000;
static const DWORD VB_RT = 0x20000;

// 自定义映射：录制状态与芯片名
static int g_recTarget = -1;          // -1 = 未录制；0-4 = 正在录制的目标
static bool g_recArmed = false;       // 已松开所有键，等待新按键
static DWORD g_recLastVirt = 0;
static wchar_t g_mapNames[5][24] = { L"A", L"B", L"X", L"", L"" };
static RECT g_mapSlotRc[5];

// 动画状态（仅界面线程访问）
static float g_animLX = 0.f, g_animLY = 0.f, g_animRX = 0.f, g_animRY = 0.f;
static int g_tick = 0;
static float g_trailLX[8] = {0}, g_trailLY[8] = {0}, g_trailRX[8] = {0}, g_trailRY[8] = {0};
static int g_trailIdx = 0;
static DWORD g_chipRelease[10] = {0};  // 按键灯熄灭动画计时（10 个灯）

#define WM_APP_SPEED (WM_APP + 1)

// ======================= 配置持久化 =======================
static void loadConfig()
{
  ConfigFile file("config.ini");
  auto num = [&](const char *key, float def) -> float {
    std::string s = file.getValueOfKey<std::string>(key);
    if (s.empty()) return def;
    return strtof(s.c_str(), 0);
  };
  g_cfgMouseLeft = (DWORD)num("CONFIG_MOUSE_LEFT", (float)XINPUT_GAMEPAD_A);
  g_cfgMouseRight = (DWORD)num("CONFIG_MOUSE_RIGHT", (float)XINPUT_GAMEPAD_B);
  g_cfgMouseMiddle = (DWORD)num("CONFIG_MOUSE_MIDDLE", (float)XINPUT_GAMEPAD_X);
  g_cfgMouseX1 = (DWORD)num("CONFIG_MOUSE_X1", 0.f);
  g_cfgMouseX2 = (DWORD)num("CONFIG_MOUSE_X2", 0.f);
  float dead = num("DEAD_ZONE", 6000.f);
  if (dead > 0.f) g_deadzone = dead;
  g_accel = num("ACCELERATION_FACTOR", 0.f);
  g_swap = num("SWAP_THUMBSTICKS", 0.f) != 0.f;
  std::string speeds = file.getValueOfKey<std::string>("CURSOR_SPEED");
  if (!speeds.empty())
  {
    size_t eq = speeds.find('=');
    std::string vs = (eq == std::string::npos) ? speeds : speeds.substr(eq + 1);
    float v = strtof(vs.c_str(), 0);
    if (v > 0.0001f && v <= 0.5f) g_speed = v;
  }
}

static void saveConfig()
{
  FILE *f = nullptr;
  if (fopen_s(&f, "config.ini", "wb") != 0 || !f) return;
  fprintf(f, "# xpad-mouse 配置（控制台版与 GUI 版共用，UTF-8）\n");
  fprintf(f, "# 按键码可相加组合；0x10000=LT 0x20000=RT（扳机）；设 0 = 禁用\n");
  fprintf(f, "CONFIG_MOUSE_LEFT = 0x%X      # 鼠标左键\n", (unsigned)g_cfgMouseLeft.load());
  fprintf(f, "CONFIG_MOUSE_RIGHT = 0x%X     # 鼠标右键\n", (unsigned)g_cfgMouseRight.load());
  fprintf(f, "CONFIG_MOUSE_MIDDLE = 0x%X    # 鼠标中键\n", (unsigned)g_cfgMouseMiddle.load());
  fprintf(f, "CONFIG_MOUSE_X1 = 0x%X        # 侧键1（网页后退）\n", (unsigned)g_cfgMouseX1.load());
  fprintf(f, "CONFIG_MOUSE_X2 = 0x%X        # 侧键2（网页前进）\n", (unsigned)g_cfgMouseX2.load());
  fprintf(f, "CONFIG_SPEED_CHANGE = 0x0300    # LB+RB = 循环切换速度\n");
  fprintf(f, "# 第一项为当前使用的灵敏度\n");
  fprintf(f, "CURSOR_SPEED = 当前=%.4f\n", (double)g_speed.load());
  fprintf(f, "DEAD_ZONE = %d\n", (int)g_deadzone.load());
  fprintf(f, "SCROLL_DEAD_ZONE = 5000\n");
  fprintf(f, "SCROLL_SPEED = 0.1\n");
  fprintf(f, "ACCELERATION_FACTOR = %.2f\n", (double)g_accel.load());
  fprintf(f, "SWAP_THUMBSTICKS = %d\n", g_swap.load() ? 1 : 0);
  fclose(f);
}

// ======================= 摇杆算法（沿用上游） =======================
static float stickMult(float lengthsq, float deadzone, float accel, int fps)
{
  float mult = (sqrtf(lengthsq) - deadzone) / (32767.f - deadzone);
  if (mult < 0.f) mult = 0.f;
  if (accel > 0.0001f) mult = powf(mult, accel);
  return mult / (float)fps;
}

static void sendMouse(DWORD flags, DWORD data = 0)
{
  INPUT in;
  ZeroMemory(&in, sizeof(in));
  in.type = INPUT_MOUSE;
  in.mi.dwFlags = flags;
  if (flags == MOUSEEVENTF_WHEEL || flags == MOUSEEVENTF_HWHEEL ||
      flags == MOUSEEVENTF_XDOWN || flags == MOUSEEVENTF_XUP)
    in.mi.mouseData = data;
  SendInput(1, &in, sizeof(INPUT));
}

// 绑定码 → 显示名（支持组合键与扳机）
static void btnName(DWORD code, wchar_t *out, int outLen)
{
  if (!code) { wcscpy_s(out, outLen, L"无"); return; }
  const struct { DWORD m; const wchar_t *n; } tbl[] = {
    { 0x1000, L"A" }, { 0x2000, L"B" }, { 0x4000, L"X" }, { 0x8000, L"Y" },
    { 0x100, L"LB" }, { 0x200, L"RB" }, { VB_LT, L"LT" }, { VB_RT, L"RT" },
    { 0x1, L"上" }, { 0x2, L"下" }, { 0x4, L"左" }, { 0x8, L"右" },
    { 0x10, L"BACK" }, { 0x20, L"START" },
  };
  out[0] = 0;
  DWORD rem = code;
  for (auto &t : tbl)
  {
    if (rem & t.m)
    {
      if (out[0]) wcscat_s(out, outLen, L"+");
      wcscat_s(out, outLen, t.n);
      rem &= ~t.m;
    }
  }
  if (rem)
  {
    wchar_t b[16];
    swprintf(b, 16, L"0x%lX", (unsigned long)rem);
    if (out[0]) wcscat_s(out, outLen, L"+");
    wcscat_s(out, outLen, b);
  }
}

static void refreshMapNames()
{
  btnName(g_cfgMouseLeft.load(), g_mapNames[0], 24);
  btnName(g_cfgMouseRight.load(), g_mapNames[1], 24);
  btnName(g_cfgMouseMiddle.load(), g_mapNames[2], 24);
  btnName(g_cfgMouseX1.load(), g_mapNames[3], 24);
  btnName(g_cfgMouseX2.load(), g_mapNames[4], 24);
}

// ======================= 后台映射线程 =======================
static const int LOOP_FPS = 150;

static DWORD WINAPI workerThread(LPVOID)
{
  CXBOXController pads[4] = { CXBOXController(1), CXBOXController(2),
                              CXBOXController(3), CXBOXController(4) };
  const int SLEEP_MS = 1000 / LOOP_FPS;
  DWORD prevVirt = 0;
  float xRest = 0.f, yRest = 0.f, wheelV = 0.f, wheelH = 0.f;
  int exitHoldMs = 0;
  bool leftDown = false, rightDown = false, midDown = false, x1Down = false, x2Down = false;
  int active = -1;
  const float presets[] = { 0.005f, 0.015f, 0.025f, 0.04f };
  const int presetCount = 4;

  auto releaseAll = [&]() {
    if (leftDown)  { sendMouse(MOUSEEVENTF_LEFTUP);          leftDown = false; }
    if (rightDown) { sendMouse(MOUSEEVENTF_RIGHTUP);         rightDown = false; }
    if (midDown)   { sendMouse(MOUSEEVENTF_MIDDLEUP);        midDown = false; }
    if (x1Down)    { sendMouse(MOUSEEVENTF_XUP, XBUTTON1);   x1Down = false; }
    if (x2Down)    { sendMouse(MOUSEEVENTF_XUP, XBUTTON2);   x2Down = false; }
  };

  while (!g_stop)
  {
    Sleep(SLEEP_MS);

    if (active >= 0 && !pads[active].IsConnected())
    {
      releaseAll();
      prevVirt = 0;
      active = -1;
    }
    if (active < 0)
    {
      for (int i = 0; i < 4; i++)
        if (pads[i].IsConnected()) { active = i; break; }
      g_padIndex = active;
      g_connected = (active >= 0);
      if (active < 0) continue;
    }

    XINPUT_STATE st = pads[active].GetState();
    WORD buttons = st.Gamepad.wButtons;

    g_lx = st.Gamepad.sThumbLX; g_ly = st.Gamepad.sThumbLY;
    g_rx = st.Gamepad.sThumbRX; g_ry = st.Gamepad.sThumbRY;
    g_buttons = buttons;
    g_lt = st.Gamepad.bLeftTrigger;
    g_rt = st.Gamepad.bRightTrigger;

    // 虚拟按键：wButtons + 扳机（>40 视为按下）
    DWORD virt = (DWORD)buttons;
    if (st.Gamepad.bLeftTrigger > 40) virt |= VB_LT;
    if (st.Gamepad.bRightTrigger > 40) virt |= VB_RT;

    bool paused = g_pauseWhenFocused && (GetForegroundWindow() == g_hwnd);

    if (g_enabled && !paused)
    {
      float speed = g_speed;
      float dead = g_deadzone;
      float accel = g_accel;
      bool swap = g_swap;

      short tx = swap ? st.Gamepad.sThumbRX : st.Gamepad.sThumbLX;
      short ty = swap ? st.Gamepad.sThumbRY : st.Gamepad.sThumbLY;

      POINT cur;
      GetCursorPos(&cur);
      float x = (float)cur.x + xRest;
      float y = (float)cur.y + yRest;
      float dx = 0.f, dy = 0.f;
      float len2 = (float)tx * tx + (float)ty * ty;
      if (len2 > dead * dead)
      {
        float mult = speed * stickMult(len2, dead, accel, LOOP_FPS);
        dx = (float)tx * mult;
        dy = (float)ty * mult;
      }
      x += dx; y -= dy;
      xRest = x - (float)((int)x);
      yRest = y - (float)((int)y);
      if (dx != 0.f || dy != 0.f) SetCursorPos((int)x, (int)y);

      short sx = swap ? st.Gamepad.sThumbLX : st.Gamepad.sThumbRX;
      short sy = swap ? st.Gamepad.sThumbLY : st.Gamepad.sThumbRY;
      float sl2 = (float)sx * sx + (float)sy * sy;
      float sdead = 5000.f;
      if (sl2 > sdead * sdead)
      {
        float mult = stickMult(sl2, sdead, 0.f, LOOP_FPS) * 0.1f;
        wheelV += (float)sy * mult;
        wheelH += (float)sx * mult;
        if (wheelV >= 1.f || wheelV <= -1.f)
        { int n = (int)wheelV; wheelV -= (float)n; sendMouse(MOUSEEVENTF_WHEEL, (DWORD)n); }
        if (wheelH >= 1.f || wheelH <= -1.f)
        { int n = (int)wheelH; wheelH -= (float)n; sendMouse(MOUSEEVENTF_HWHEEL, (DWORD)n); }
      }
      else { wheelV = 0.f; wheelH = 0.f; }

      auto edge = [&](DWORD code) -> int {
        if (!code) return 0;
        bool d = (virt & code) == code, w = (prevVirt & code) == code;
        return (d && !w) ? 1 : (!d && w) ? 2 : 0;
      };
      DWORD wl = g_cfgMouseLeft.load(), wr = g_cfgMouseRight.load();
      DWORD wm = g_cfgMouseMiddle.load(), w1 = g_cfgMouseX1.load(), w2 = g_cfgMouseX2.load();
      if (edge(wl))
      { bool d = (virt & wl) == wl; sendMouse(d ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP); leftDown = d; }
      if (edge(wr))
      { bool d = (virt & wr) == wr; sendMouse(d ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP); rightDown = d; }
      if (wm && edge(wm))
      { bool d = (virt & wm) == wm; sendMouse(d ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP); midDown = d; }
      if (w1 && edge(w1))
      { bool d = (virt & w1) == w1; sendMouse(d ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, XBUTTON1); x1Down = d; }
      if (w2 && edge(w2))
      { bool d = (virt & w2) == w2; sendMouse(d ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, XBUTTON2); x2Down = d; }

      if (edge(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER) == 1)
      {
        int idx = (g_speedPreset + 1) % presetCount;
        g_speedPreset = idx;
        g_speed = presets[idx];
        pads[active].Vibrate(40000, 40000);
        Sleep(80);
        pads[active].Vibrate(0, 0);
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_SPEED, idx, 0);
      }
    }

    if ((virt & (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START)) ==
        (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START))
    {
      exitHoldMs += SLEEP_MS;
      if (exitHoldMs >= 1000)
      {
        if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        exitHoldMs = 0;
      }
    }
    else exitHoldMs = 0;

    prevVirt = virt;
  }

  releaseAll();
  return 0;
}

// ======================= 控件 =======================
enum CtrlId { IDB_TOGGLE = 100, IDB_SAVE, IDB_DEFAULT, IDC_PAUSE, IDC_SWAP };
enum CtrlKind { CK_BUTTON, CK_SWITCH };

struct Ctrl
{
  int id = 0;
  CtrlKind kind = CK_BUTTON;
  RECT rc{};
  wchar_t text[48] = L"";
  bool hover = false;
  bool checked = false;
  float anim = 0.f;      // 开关滑钮动画位置 0..1
  bool primary = false;  // 主按钮样式
};

static std::vector<Ctrl> g_ctrls;
static int g_dragSlider = -1;

static Ctrl *findCtrl(int id)
{
  for (auto &c : g_ctrls) if (c.id == id) return &c;
  return nullptr;
}

// ======================= 布局 =======================
static int g_dpi = 96;
static int sc(int v) { return MulDiv(v, g_dpi, 96); }

static RECT g_rcSpeed, g_rcDead, g_rcAccel;
static RECT g_rcCardL, g_rcCardR, g_rcPreview, g_rcStatus, g_rcMap;

static void layout(int w, int h)
{
  int pad = sc(24);
  g_rcCardL = { pad, sc(92), pad + sc(548), sc(92) + sc(430) };
  g_rcCardR = { pad + sc(548) + sc(20), sc(92), w - pad, sc(92) + sc(430) };

  int ix = g_rcCardL.left + sc(24);
  int y = g_rcCardL.top + sc(64);
  g_rcSpeed = { ix, y, g_rcCardL.right - sc(24), y + sc(44) }; y += sc(68);
  g_rcDead  = { ix, y, g_rcCardL.right - sc(24), y + sc(44) }; y += sc(68);
  g_rcAccel = { ix, y, g_rcCardL.right - sc(24), y + sc(44) };

  int sy = g_rcCardL.bottom - sc(88);
  auto put = [&](int id, RECT rc) { for (auto &c : g_ctrls) if (c.id == id) c.rc = rc; };
  put(IDC_PAUSE, { ix, sy, ix + sc(260), sy + sc(28) });
  put(IDC_SWAP,  { ix + sc(276), sy, ix + sc(524), sy + sc(28) });

  int by = g_rcCardL.bottom - sc(52);
  put(IDB_TOGGLE,  { ix, by, ix + sc(168), by + sc(32) });
  put(IDB_SAVE,    { ix + sc(180), by, ix + sc(328), by + sc(32) });
  put(IDB_DEFAULT, { ix + sc(340), by, ix + sc(524), by + sc(32) });

  g_rcPreview = { g_rcCardR.left + sc(18), g_rcCardR.top + sc(52),
                  g_rcCardR.right - sc(18), g_rcCardR.top + sc(286) };

  // 按键映射卡片（两卡下方整行）
  int my = g_rcCardL.bottom + sc(16);
  g_rcMap = { pad, my, w - pad, my + sc(104) };
  int sx2 = g_rcMap.left + sc(24);
  int sw = sc(164), sgap = sc(10);
  for (int i = 0; i < 5; i++)
    g_mapSlotRc[i] = { sx2 + i * (sw + sgap), g_rcMap.top + sc(48),
                       sx2 + i * (sw + sgap) + sw, g_rcMap.top + sc(48) + sc(40) };

  g_rcStatus = { pad, h - sc(64), w - pad, h - sc(20) };
}

// ======================= 绘制辅助 =======================
static void fillRound(HDC hdc, RECT rc, COLORREF c, int r)
{
  HBRUSH b = CreateSolidBrush(c);
  HPEN p = CreatePen(PS_SOLID, 1, c);
  HBRUSH ob = (HBRUSH)SelectObject(hdc, b);
  HPEN op = (HPEN)SelectObject(hdc, p);
  RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, r, r);
  SelectObject(hdc, ob); SelectObject(hdc, op);
  DeleteObject(b); DeleteObject(p);
}

static void frameRound(HDC hdc, RECT rc, COLORREF c, int r, int width = 1)
{
  HPEN p = CreatePen(PS_SOLID, width, c);
  HBRUSH ob = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
  HPEN op = (HPEN)SelectObject(hdc, p);
  RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, r, r);
  SelectObject(hdc, ob); SelectObject(hdc, op);
  DeleteObject(p);
}

static void gradRound(HDC hdc, RECT rc, COLORREF top, COLORREF bottom, int r)
{
  // 用两层 RoundRect 模拟竖向渐变（上半用 top，下半用 bottom，中线用混合色近似）
  fillRound(hdc, rc, top, r);
  RECT lo = rc;
  lo.top = (rc.top + rc.bottom) / 2;
  HBRUSH b = CreateSolidBrush(bottom);
  HPEN p = CreatePen(PS_SOLID, 1, bottom);
  HBRUSH ob = (HBRUSH)SelectObject(hdc, b);
  HPEN op = (HPEN)SelectObject(hdc, p);
  RoundRect(hdc, lo.left, lo.top, lo.right, lo.bottom, r, r);
  SelectObject(hdc, ob); SelectObject(hdc, op);
  DeleteObject(b); DeleteObject(p);
}

static void textOut(HDC hdc, int x, int y, const wchar_t *s, HFONT f, COLORREF c,
                    int align = TA_LEFT | TA_TOP)
{
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, c);
  HFONT of = (HFONT)SelectObject(hdc, f);
  SIZE sz;
  GetTextExtentPoint32W(hdc, s, (int)wcslen(s), &sz);
  int tx = x;
  if ((align & TA_CENTER) == TA_CENTER) tx = x - sz.cx / 2;
  else if ((align & TA_RIGHT) == TA_RIGHT) tx = x - sz.cx;
  TextOutW(hdc, tx, y, s, (int)wcslen(s));
  SelectObject(hdc, of);
}

static int textWidth(HDC hdc, const wchar_t *s, HFONT f)
{
  HFONT of = (HFONT)SelectObject(hdc, f);
  SIZE sz;
  GetTextExtentPoint32W(hdc, s, (int)wcslen(s), &sz);
  SelectObject(hdc, of);
  return sz.cx;
}

// 卡片：底色 + 边框 + 顶部高光线
static void drawCard(HDC hdc, RECT rc)
{
  fillRound(hdc, rc, C_CARD, sc(12));
  frameRound(hdc, rc, C_BORDER_DK, sc(12));
  HPEN pen = CreatePen(PS_SOLID, 1, RGB(46, 54, 70));
  HPEN op = (HPEN)SelectObject(hdc, pen);
  MoveToEx(hdc, rc.left + sc(12), rc.top + 1, nullptr);
  LineTo(hdc, rc.right - sc(12), rc.top + 1);
  SelectObject(hdc, op);
  DeleteObject(pen);
}

static const wchar_t *speedWord(float v)
{
  if (v < 0.008f) return L"极慢";
  if (v < 0.018f) return L"慢";
  if (v < 0.035f) return L"中";
  if (v < 0.07f) return L"快";
  return L"极快";
}

// ======================= 滑条 =======================
static void drawSlider(HDC hdc, RECT trackArea, const wchar_t *label, const wchar_t *hint,
                       std::atomic<float> *val, float mn, float mx, bool isSpeed)
{
  int x = trackArea.left, right = trackArea.right;
  int top = trackArea.top;

  textOut(hdc, x, top, label, g_font, C_TEXT);
  if (hint && hint[0])
  {
    // 灵敏度的档位字用强调色，其余提示用暗色
    COLORREF hc = isSpeed ? C_ACCENT : C_DIM;
    HFONT hf = isSpeed ? g_font : g_fontSmall;
    int hy = isSpeed ? top : top + sc(3);
    textOut(hdc, x + textWidth(hdc, label, g_font) + sc(10), hy, hint, hf, hc);
  }

  // 值徽章（纯数字，等宽字体）
  wchar_t buf[64];
  if (isSpeed) swprintf(buf, 64, L"%.3f", (double)val->load());
  else if (val == &g_deadzone) swprintf(buf, 64, L"%d", (int)val->load());
  else swprintf(buf, 64, L"%.1f", (double)val->load());
  int bw = textWidth(hdc, buf, g_fontNum) + sc(22);
  RECT badge = { right - bw, top - sc(2), right, top + sc(22) };
  fillRound(hdc, badge, C_INSET, sc(6));
  frameRound(hdc, badge, C_BORDER_DK, sc(6));
  textOut(hdc, (badge.left + badge.right) / 2, top + sc(2), buf, g_fontNum, C_ACCENT,
          TA_CENTER | TA_TOP);

  // 轨道
  int ty = top + sc(30);
  RECT track = { x, ty, right - sc(0), ty + sc(8) };
  fillRound(hdc, track, C_INSET, sc(4));
  frameRound(hdc, track, C_BORDER_DK, sc(4));

  float t = (val->load() - mn) / (mx - mn);
  if (t < 0.f) t = 0.f;
  if (t > 1.f) t = 1.f;
  int fillW = (int)((track.right - track.left - sc(8)) * t);
  if (fillW > 0)
  {
    RECT fr = { track.left + sc(4), track.top + sc(2), track.left + sc(4) + fillW, track.bottom - sc(2) };
    HBRUSH b1 = CreateSolidBrush(C_ACCENT);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, b1);
    HPEN op = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
    RoundRect(hdc, fr.left, fr.top, fr.right, fr.bottom, sc(3), sc(3));
    SelectObject(hdc, ob); SelectObject(hdc, op);
    DeleteObject(b1);
  }

  // 滑块（辉光 + 圆钮）
  int thumbX = track.left + sc(4) + fillW;
  int cy = ty + sc(4);
  drawGlow(hdc, g_glowGreen, thumbX, cy, sc(16), 110);
  HBRUSH b = CreateSolidBrush(C_THUMB);
  HPEN p = CreatePen(PS_SOLID, 1, C_ACCENT);
  HBRUSH ob = (HBRUSH)SelectObject(hdc, b);
  HPEN op = (HPEN)SelectObject(hdc, p);
  int r = sc(7);
  Ellipse(hdc, thumbX - r, cy - r, thumbX + r, cy + r);
  SelectObject(hdc, ob); SelectObject(hdc, op);
  DeleteObject(b); DeleteObject(p);
}

static int hitSlider(int x, int y)
{
  RECT r;
  if (PtInRect(&g_rcSpeed, { x, y })) return 0;
  if (PtInRect(&g_rcDead, { x, y })) return 1;
  if (PtInRect(&g_rcAccel, { x, y })) return 2;
  (void)r;
  return -1;
}

static void sliderSetFromX(int idx, int x)
{
  RECT r = idx == 0 ? g_rcSpeed : idx == 1 ? g_rcDead : g_rcAccel;
  float t = (float)(x - r.left) / (float)(r.right - r.left);
  if (t < 0.f) t = 0.f;
  if (t > 1.f) t = 1.f;
  if (idx == 0) g_speed = 0.002f + t * (0.150f - 0.002f);
  else if (idx == 1) g_deadzone = t * 15000.f;
  else
  {
    g_accel = t * 4.f;
    if (g_accel < 0.15f) g_accel = 0.f;
  }
}

// ======================= 主绘制 =======================
static void paint(HWND hwnd)
{
  PAINTSTRUCT ps;
  HDC hdc = BeginPaint(hwnd, &ps);
  RECT rc;
  GetClientRect(hwnd, &rc);
  int W = rc.right, H = rc.bottom;

  HDC mem = CreateCompatibleDC(hdc);
  HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
  HBITMAP obmp = (HBITMAP)SelectObject(mem, bmp);

  // 背景（顶部稍亮的竖向渐变）
  {
    RECT a = { 0, 0, W, H / 3 }, b = { 0, H / 3, W, H };
    HBRUSH ba = CreateSolidBrush(C_BG_TOP), bb = CreateSolidBrush(C_BG);
    FillRect(mem, &a, ba);
    FillRect(mem, &b, bb);
    DeleteObject(ba); DeleteObject(bb);
  }

  // ---- 头部 ----
  {
    RECT logo = { sc(24), sc(22), sc(24) + sc(40), sc(22) + sc(40) };
    drawGlow(mem, g_glowGreen, (logo.left + logo.right) / 2, (logo.top + logo.bottom) / 2, sc(28), 80);
    gradRound(mem, logo, C_ACCENT, C_ACCENT_DK, sc(10));
    textOut(mem, (logo.left + logo.right) / 2, logo.top + sc(7), L"X", g_fontHead, RGB(10, 26, 18),
            TA_CENTER | TA_TOP);

    textOut(mem, logo.right + sc(14), sc(20), L"xpad-mouse", g_fontTitle, C_TEXT);
    textOut(mem, logo.right + sc(14), sc(50), L"手柄摇杆映射鼠标  ·  基于 Gopher360 改装 (GPL-3.0)",
            g_fontSmall, C_DIM);

    // 右上角映射开关状态
    bool on = g_enabled;
    RECT pill = { W - sc(150), sc(28), W - sc(24), sc(54) };
    fillRound(mem, pill, on ? RGB(24, 48, 36) : C_CARD, sc(13));
    frameRound(mem, pill, on ? C_ACCENT : C_BORDER, sc(13));
    textOut(mem, (pill.left + pill.right) / 2, sc(32), on ? L"● 映射开启" : L"○ 映射关闭",
            g_fontSmall, on ? C_ACCENT : C_DIM, TA_CENTER | TA_TOP);
  }

  // ---- 左卡片 ----
  drawCard(mem, g_rcCardL);
  {
    int ix = g_rcCardL.left + sc(24);
    textOut(mem, ix, g_rcCardL.top + sc(20), L"参数调节", g_font, C_TEXT);
    textOut(mem, ix + sc(80), g_rcCardL.top + sc(23), L"拖动滑条，实时生效", g_fontSmall, C_DIM);

    drawSlider(mem, g_rcSpeed, L"指针灵敏度", speedWord(g_speed.load()), &g_speed, 0.002f, 0.150f, true);
    drawSlider(mem, g_rcDead, L"摇杆死区", L"指针漂移就调大", &g_deadzone, 0.f, 15000.f, false);
    drawSlider(mem, g_rcAccel, L"加速曲线", L"0 = 线性", &g_accel, 0.f, 4.f, false);
  }

  // ---- 右卡片：预览 ----
  drawCard(mem, g_rcCardR);
  {
    textOut(mem, g_rcCardR.left + sc(18), g_rcCardR.top + sc(18), L"实时预览", g_font, C_TEXT);

    RECT p = g_rcPreview;
    fillRound(mem, p, C_INSET, sc(10));
    frameRound(mem, p, C_BORDER_DK, sc(10));

    int cx = (p.left + p.right) / 2;
    int cy = (p.top + p.bottom) / 2 + sc(6);
    int R = __min((p.right - p.left) / 2 - sc(18), (p.bottom - p.top) / 2 - sc(14));

    // 点阵背景
    for (int gy = p.top + sc(12); gy < p.bottom - sc(8); gy += sc(20))
      for (int gx = p.left + sc(12); gx < p.right - sc(8); gx += sc(20))
        SetPixel(mem, gx, gy, C_GRID);

    // 外圈 + 十字线
    {
      HPEN pen = CreatePen(PS_SOLID, 1, C_BORDER);
      HBRUSH ob = (HBRUSH)SelectObject(mem, GetStockObject(NULL_BRUSH));
      HPEN op = (HPEN)SelectObject(mem, pen);
      Ellipse(mem, cx - R, cy - R, cx + R, cy + R);
      HPEN pen2 = CreatePen(PS_SOLID, 1, RGB(32, 40, 54));
      SelectObject(mem, pen2);
      MoveToEx(mem, cx - R, cy, nullptr); LineTo(mem, cx + R, cy);
      MoveToEx(mem, cx, cy - R, nullptr); LineTo(mem, cx, cy + R);
      SelectObject(mem, ob); SelectObject(mem, op);
      DeleteObject(pen); DeleteObject(pen2);
    }

    // 死区环
    int dr = (int)(R * (g_deadzone.load() / 32767.f));
    if (dr > 3)
    {
      HPEN pen = CreatePen(PS_DOT, 1, C_DIM);
      HBRUSH ob = (HBRUSH)SelectObject(mem, GetStockObject(NULL_BRUSH));
      HPEN op = (HPEN)SelectObject(mem, pen);
      Ellipse(mem, cx - dr, cy - dr, cx + dr, cy + dr);
      SelectObject(mem, ob); SelectObject(mem, op);
      DeleteObject(pen);
    }

    // 拖尾 + 摇杆点
    auto stick = [&](float ax, float ay, const GlowSprite &glow, COLORREF core) {
      int dx = cx + (int)(ax / 32767.f * (float)R);
      int dy = cy - (int)(ay / 32767.f * (float)R);
      drawGlow(mem, glow, dx, dy, sc(20), 150);
      HBRUSH b = CreateSolidBrush(core);
      HPEN p2 = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
      HBRUSH ob = (HBRUSH)SelectObject(mem, b);
      HPEN op = (HPEN)SelectObject(mem, p2);
      int r = sc(5);
      Ellipse(mem, dx - r, dy - r, dx + r, dy + r);
      SelectObject(mem, ob); SelectObject(mem, op);
      DeleteObject(b); DeleteObject(p2);
    };
    // 拖尾（低透明度小点，画在点下面）
    for (int i = 0; i < 8; i++)
    {
      int k = (g_trailIdx + i) % 8;
      int alpha = (i + 1) * 12;
      int lx = cx + (int)(g_trailLX[k] / 32767.f * (float)R);
      int ly = cy - (int)(g_trailLY[k] / 32767.f * (float)R);
      drawGlow(mem, g_glowGreen, lx, ly, sc(7), (BYTE)alpha);
      int rx = cx + (int)(g_trailRX[k] / 32767.f * (float)R);
      int ry = cy - (int)(g_trailRY[k] / 32767.f * (float)R);
      drawGlow(mem, g_glowBlue, rx, ry, sc(7), (BYTE)alpha);
    }
    stick(g_animRX, g_animRY, g_glowBlue, C_BLUE);
    stick(g_animLX, g_animLY, g_glowGreen, C_ACCENT);

    // 图例
    int ly = p.bottom + sc(8);
    textOut(mem, p.left + sc(4), ly, L"●", g_fontSmall, C_ACCENT);
    textOut(mem, p.left + sc(18), ly, L"左摇杆 · 移动指针", g_fontSmall, C_DIM);
    textOut(mem, p.left + sc(150), ly, L"●", g_fontSmall, C_BLUE);
    textOut(mem, p.left + sc(164), ly, L"右摇杆 · 滚轮", g_fontSmall, C_DIM);

    // 按键 LED（含扳机）
    unsigned short bs = g_buttons.load();
    int ltv = g_lt.load(), rtv = g_rt.load();
    struct Chip { const wchar_t *t; DWORD mask; COLORREF col; };
    Chip chips[] = {
      { L"A", XINPUT_GAMEPAD_A, C_ACCENT }, { L"B", XINPUT_GAMEPAD_B, C_ACCENT },
      { L"X", XINPUT_GAMEPAD_X, C_ACCENT }, { L"Y", XINPUT_GAMEPAD_Y, C_AMBER },
      { L"LB", XINPUT_GAMEPAD_LEFT_SHOULDER, C_BLUE }, { L"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER, C_BLUE },
      { L"LT", VB_LT, C_AMBER }, { L"RT", VB_RT, C_AMBER },
      { L"BACK", XINPUT_GAMEPAD_BACK, C_DIM }, { L"START", XINPUT_GAMEPAD_START, C_DIM },
    };
    int cw = sc(52), chh = sc(24), gap = sc(7);
    int bx = p.left + sc(4);
    int byy = g_rcCardR.bottom - sc(74);
    for (int i = 0; i < 10; i++)
    {
      int col = i % 5, row = i / 5;
      RECT cr = { bx + col * (cw + gap), byy + row * (chh + gap),
                  bx + col * (cw + gap) + cw, byy + row * (chh + gap) + chh };
      bool lit = (chips[i].mask == VB_LT) ? (ltv > 40)
               : (chips[i].mask == VB_RT) ? (rtv > 40)
               : ((bs & (WORD)chips[i].mask) != 0);
      if (lit) g_chipRelease[i] = 0;
      else if (g_chipRelease[i] < 20) g_chipRelease[i]++;

      BYTE glowA = lit ? 160 : (BYTE)(g_chipRelease[i] < 12 ? (12 - g_chipRelease[i]) * 10 : 0);
      if (glowA > 0)
        drawGlow(mem, chips[i].col == C_DIM ? g_glowWhite : (chips[i].col == C_BLUE ? g_glowBlue :
                 chips[i].col == C_AMBER ? g_glowAmber : g_glowGreen),
                 (cr.left + cr.right) / 2, (cr.top + cr.bottom) / 2, sc(20), glowA);
      fillRound(mem, cr, lit ? chips[i].col : C_INSET, sc(6));
      if (!lit) frameRound(mem, cr, C_BORDER_DK, sc(6));
      COLORREF tc = lit ? RGB(10, 16, 12) : C_DIM;
      textOut(mem, (cr.left + cr.right) / 2, cr.top + sc(3), chips[i].t, g_fontSmall, tc,
              TA_CENTER | TA_TOP);
    }
  }

  // ---- 按键映射卡片 ----
  drawCard(mem, g_rcMap);
  {
    textOut(mem, g_rcMap.left + sc(24), g_rcMap.top + sc(14), L"按键映射", g_font, C_TEXT);
    const wchar_t *hint = (g_recTarget >= 0)
        ? L"按住扳机也算：按下或组合手柄上的键即可绑定 · 先松开所有键再按新的 · 点其他槽位或 ESC 取消"
        : L"点击右侧芯片，再按手柄上的键即可重新绑定（支持 LT/RT、十字键、组合键）";
    textOut(mem, g_rcMap.left + sc(110), g_rcMap.top + sc(17), hint, g_fontSmall,
            (g_recTarget >= 0) ? C_AMBER : C_DIM);

    const wchar_t *labels[5] = { L"左键", L"右键", L"中键", L"侧键1", L"侧键2" };
    for (int i = 0; i < 5; i++)
    {
      RECT slot = g_mapSlotRc[i];
      bool rec = (g_recTarget == i);
      fillRound(mem, slot, rec ? RGB(42, 36, 18) : C_INSET, sc(8));
      frameRound(mem, slot, rec ? C_AMBER : C_BORDER_DK, sc(8));
      textOut(mem, slot.left + sc(12), slot.top + sc(10), labels[i], g_fontSmall, C_DIM);

      // 绑定芯片（右侧）
      int chipW = sc(84);
      RECT chip = { slot.right - chipW - sc(6), slot.top + sc(6),
                    slot.right - sc(6), slot.bottom - sc(6) };
      COLORREF chipFill, chipText;
      if (rec)
      {
        bool blink = (g_tick / 12) % 2 == 0;
        chipFill = blink ? C_AMBER : RGB(80, 62, 26);
        chipText = rec ? (blink ? RGB(20, 16, 8) : C_AMBER) : C_DIM;
        drawGlow(mem, g_glowAmber, (chip.left + chip.right) / 2, (chip.top + chip.bottom) / 2,
                 sc(26), 120);
      }
      else
      {
        chipFill = C_CARD;
        bool isNone = (wcscmp(g_mapNames[i], L"无") == 0);
        chipText = isNone ? C_DIM : C_ACCENT;
      }
      fillRound(mem, chip, chipFill, sc(6));
      if (!rec) frameRound(mem, chip, chipText == C_ACCENT ? C_ACCENT_DK : C_BORDER, sc(6));
      const wchar_t *show = rec ? L"等待按键…" : g_mapNames[i];
      textOut(mem, (chip.left + chip.right) / 2, chip.top + sc(4), show, g_fontSmall, chipText,
              TA_CENTER | TA_TOP);
    }
  }

  // ---- 控件：开关 + 按钮 ----
  for (auto &c : g_ctrls)
  {
    if (c.kind == CK_SWITCH)
    {
      // 标签
      textOut(mem, c.rc.left, c.rc.top + sc(2), c.text, g_fontSmall, C_TEXT);
      // 胶囊
      int sw = sc(40), shh = sc(20);
      RECT pill = { c.rc.right - sw, c.rc.top + sc(1), c.rc.right, c.rc.top + sc(1) + shh };
      COLORREF fill = c.checked ? C_ACCENT : C_INSET;
      fillRound(mem, pill, fill, sc(10));
      frameRound(mem, pill, c.checked ? C_ACCENT_DK : C_BORDER, sc(10));
      // 滑钮（带动画）
      float target = c.checked ? 1.f : 0.f;
      c.anim += (target - c.anim) * 0.35f;
      int kx = pill.left + sc(3) + (int)((sw - sc(6) - sc(2) * 2) * c.anim) + sc(2);
      int ky = (pill.top + pill.bottom) / 2;
      if (c.checked) drawGlow(mem, g_glowGreen, kx, ky, sc(12), 120);
      HBRUSH b = CreateSolidBrush(c.checked ? RGB(12, 30, 20) : C_THUMB);
      HBRUSH ob = (HBRUSH)SelectObject(mem, b);
      HPEN op = (HPEN)SelectObject(mem, GetStockObject(NULL_PEN));
      int kr = sc(7);
      Ellipse(mem, kx - kr, ky - kr, kx + kr, ky + kr);
      SelectObject(mem, ob); SelectObject(mem, op);
      DeleteObject(b);
    }
    else if (c.kind == CK_BUTTON)
    {
      bool disabled = false;
      if (c.id == IDB_TOGGLE && !g_enabled)
      {
        // 关闭态：描边幽灵按钮
        fillRound(mem, c.rc, c.hover ? C_CARD : C_INSET, sc(8));
        frameRound(mem, c.rc, c.hover ? C_ACCENT : C_BORDER, sc(8));
        textOut(mem, (c.rc.left + c.rc.right) / 2, c.rc.top + sc(7), c.text, g_font,
                c.hover ? C_ACCENT : C_TEXT, TA_CENTER | TA_TOP);
        continue;
      }
      if (c.id == IDB_TOGGLE)
      {
        COLORREF t = c.hover ? RGB(78, 236, 158) : C_ACCENT;
        COLORREF bo = c.hover ? RGB(78, 236, 158) : C_ACCENT_DK;
        int cx2 = (c.rc.left + c.rc.right) / 2, cy2 = (c.rc.top + c.rc.bottom) / 2;
        drawGlow(mem, g_glowGreen, cx2, cy2, sc(30), c.hover ? 150 : 90);
        gradRound(mem, c.rc, t, bo, sc(8));
        textOut(mem, cx2, c.rc.top + sc(7), c.text, g_font, RGB(10, 26, 18), TA_CENTER | TA_TOP);
      }
      else
      {
        fillRound(mem, c.rc, c.hover ? RGB(40, 47, 62) : C_INSET, sc(8));
        frameRound(mem, c.rc, c.hover ? C_ACCENT : C_BORDER, sc(8));
        textOut(mem, (c.rc.left + c.rc.right) / 2, c.rc.top + sc(7), c.text, g_font,
                c.hover ? C_ACCENT : C_TEXT, TA_CENTER | TA_TOP);
      }
      (void)disabled;
    }
  }

  // ---- 状态条 ----
  {
    RECT s = g_rcStatus;
    drawCard(mem, s);
    bool conn = g_connected.load();
    float pulse = 0.62f + 0.38f * sinf(g_tick * 0.12f);
    int dotx = s.left + sc(20), doty = (s.top + s.bottom) / 2;
    if (conn)
      drawGlow(mem, g_glowGreen, dotx, doty, sc(14), (BYTE)(pulse * 200));
    HBRUSH b = CreateSolidBrush(conn ? C_ACCENT : C_DIM);
    HBRUSH ob = (HBRUSH)SelectObject(mem, b);
    HPEN op = (HPEN)SelectObject(mem, GetStockObject(NULL_PEN));
    int r = sc(4);
    Ellipse(mem, dotx - r, doty - r, dotx + r, doty + r);
    SelectObject(mem, ob); SelectObject(mem, op);
    DeleteObject(b);

    wchar_t st[160];
    bool focused = g_pauseWhenFocused.load() && (GetForegroundWindow() == g_hwnd);
    if (conn)
      swprintf(st, 160, L"手柄已连接 (#%d)   ·   灵敏度 %.3f   ·   %ls%ls",
               g_padIndex.load() + 1, (double)g_speed.load(),
               g_enabled.load() ? L"映射中" : L"已暂停",
               focused ? L"（本窗口聚焦，已自动暂停）" : L"");
    else
      swprintf(st, 160, L"等待手柄连接…  请确认手柄开机或接收器已插好");
    textOut(mem, s.left + sc(36), s.top + sc(12), st, g_font, conn ? C_TEXT : C_AMBER);
    textOut(mem, s.right - sc(14), s.top + sc(13), L"BACK+START 按住1秒退出  ·  LB+RB 切换速度",
            g_fontSmall, C_DIM, TA_RIGHT | TA_TOP);
  }

  BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
  SelectObject(mem, obmp);
  DeleteObject(bmp);
  DeleteDC(mem);
  EndPaint(hwnd, &ps);
}

// ======================= 窗口过程 =======================
static HFONT makeFont(int ptBold, int dpi, const wchar_t *face)
{
  return CreateFontW(-MulDiv(ptBold, dpi, 72), 0, 0, 0, ptBold >= 600 ? FW_BOLD : FW_NORMAL,
                     FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

static void makeFonts(int dpi)
{
  if (g_fontTitle) DeleteObject(g_fontTitle);
  if (g_fontHead) DeleteObject(g_fontHead);
  if (g_font) DeleteObject(g_font);
  if (g_fontSmall) DeleteObject(g_fontSmall);
  if (g_fontNum) DeleteObject(g_fontNum);
  g_fontTitle = makeFont(700, dpi, L"Segoe UI");
  g_fontTitle = CreateFontW(-MulDiv(22, dpi, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  g_fontHead = CreateFontW(-MulDiv(18, dpi, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  g_font = CreateFontW(-MulDiv(13, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
  g_fontSmall = CreateFontW(-MulDiv(11, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
  g_fontNum = CreateFontW(-MulDiv(13, dpi, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
  case WM_CREATE:
  {
    HDC scr = GetDC(hwnd);
    g_dpi = GetDeviceCaps(scr, LOGPIXELSX);
    ReleaseDC(hwnd, scr);
    makeFonts(g_dpi);

    g_glowGreen = makeGlow(RGB(56, 224, 139));
    g_glowBlue = makeGlow(RGB(86, 194, 255));
    g_glowWhite = makeGlow(RGB(230, 236, 248));
    g_glowAmber = makeGlow(RGB(255, 180, 74));

    g_ctrls.clear();
    Ctrl c{};
    c.kind = CK_SWITCH; c.id = IDC_PAUSE; wcscpy(c.text, L"本窗口聚焦时暂停映射");
    c.checked = true; c.anim = 1.f;
    g_ctrls.push_back(c);
    c = {}; c.kind = CK_SWITCH; c.id = IDC_SWAP; wcscpy(c.text, L"交换摇杆");
    c.checked = g_swap.load(); c.anim = c.checked ? 1.f : 0.f;
    g_ctrls.push_back(c);
    c = {}; c.kind = CK_BUTTON; c.id = IDB_TOGGLE; c.primary = true; wcscpy(c.text, L"暂停映射");
    g_ctrls.push_back(c);
    c = {}; c.kind = CK_BUTTON; c.id = IDB_SAVE; wcscpy(c.text, L"保存配置");
    g_ctrls.push_back(c);
    c = {}; c.kind = CK_BUTTON; c.id = IDB_DEFAULT; wcscpy(c.text, L"恢复默认");
    g_ctrls.push_back(c);

    g_thread = CreateThread(nullptr, 0, workerThread, nullptr, 0, nullptr);
    refreshMapNames();
    SetTimer(hwnd, 1, 33, nullptr);
    return 0;
  }

  case WM_SIZE:
    layout(LOWORD(lp), HIWORD(lp));
    return 0;

  case WM_GETMINMAXINFO:
  {
    MINMAXINFO *mi = (MINMAXINFO *)lp;
    mi->ptMinTrackSize.x = sc(760);
    mi->ptMinTrackSize.y = sc(760);
    return 0;
  }

  case WM_TIMER:
  {
    g_tick++;
    // 摇杆点弹性跟随
    const float K = 0.42f;
    g_animLX += ((float)g_lx.load() - g_animLX) * K;
    g_animLY += ((float)g_ly.load() - g_animLY) * K;
    g_animRX += ((float)g_rx.load() - g_animRX) * K;
    g_animRY += ((float)g_ry.load() - g_animRY) * K;
    // 拖尾采样
    g_trailLX[g_trailIdx] = g_animLX; g_trailLY[g_trailIdx] = g_animLY;
    g_trailRX[g_trailIdx] = g_animRX; g_trailRY[g_trailIdx] = g_animRY;
    g_trailIdx = (g_trailIdx + 1) % 8;

    // 按键录制轮询
    if (g_recTarget >= 0)
    {
      DWORD virt = (DWORD)g_buttons.load();
      if (g_lt.load() > 40) virt |= VB_LT;
      if (g_rt.load() > 40) virt |= VB_RT;
      if (!g_recArmed)
      {
        if (virt == 0) g_recArmed = true;
      }
      else if (virt != 0)
      {
        DWORD pressed = virt & ~g_recLastVirt;
        if (pressed)
        {
          DWORD bit = pressed & (~pressed + 1);   // 取最低位（单键或组合中的第一个）
          switch (g_recTarget)
          {
            case 0: g_cfgMouseLeft = bit; break;
            case 1: g_cfgMouseRight = bit; break;
            case 2: g_cfgMouseMiddle = bit; break;
            case 3: g_cfgMouseX1 = bit; break;
            case 4: g_cfgMouseX2 = bit; break;
          }
          g_recTarget = -1;
          g_recArmed = false;
          refreshMapNames();
        }
      }
      g_recLastVirt = virt;
    }

    InvalidateRect(hwnd, nullptr, FALSE);
    return 0;
  }

  case WM_APP_SPEED:
    InvalidateRect(hwnd, nullptr, FALSE);
    return 0;

  case WM_MOUSEMOVE:
  {
    int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
    bool dirty = false;
    if (g_dragSlider >= 0) { sliderSetFromX(g_dragSlider, x); dirty = true; }
    for (auto &c : g_ctrls)
    {
      bool h = PtInRect(&c.rc, { x, y });
      if (h != c.hover) { c.hover = h; dirty = true; }
    }
    if (dirty) InvalidateRect(hwnd, nullptr, FALSE);
    return 0;
  }

  case WM_LBUTTONDOWN:
  {
    int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
    SetCapture(hwnd);

    // 映射槽位：点击进入/切换录制
    for (int i = 0; i < 5; i++)
    {
      if (PtInRect(&g_mapSlotRc[i], { x, y }))
      {
        if (g_recTarget == i)
        {
          g_recTarget = -1;      // 再点一次取消
        }
        else
        {
          g_recTarget = i;
          g_recArmed = false;
          g_recLastVirt = 0;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
    }

    for (auto &c : g_ctrls)
    {
      if (!PtInRect(&c.rc, { x, y })) continue;
      if (c.kind == CK_BUTTON)
      {
        if (c.id == IDB_TOGGLE)
        {
          g_enabled = !g_enabled;
          wcscpy(c.text, g_enabled ? L"暂停映射" : L"继续映射");
        }
        else if (c.id == IDB_SAVE)
        {
          saveConfig();
          wcscpy(c.text, L"已保存 ✓");
          SetTimer(hwnd, 200, 1200, [](HWND h, UINT, UINT_PTR id, DWORD) {
            KillTimer(h, id);
            if (Ctrl *cc = findCtrl(IDB_SAVE)) wcscpy(cc->text, L"保存配置");
            InvalidateRect(h, nullptr, FALSE);
          });
        }
        else if (c.id == IDB_DEFAULT)
        {
          g_speed = 0.03f; g_deadzone = 6000.f; g_accel = 0.f; g_swap = false;
          g_cfgMouseLeft = XINPUT_GAMEPAD_A; g_cfgMouseRight = XINPUT_GAMEPAD_B;
          g_cfgMouseMiddle = XINPUT_GAMEPAD_X; g_cfgMouseX1 = 0; g_cfgMouseX2 = 0;
          refreshMapNames();
          if (Ctrl *sw = findCtrl(IDC_SWAP)) sw->checked = false;
        }
      }
      else if (c.kind == CK_SWITCH)
      {
        c.checked = !c.checked;
        if (c.id == IDC_PAUSE) g_pauseWhenFocused = c.checked;
        if (c.id == IDC_SWAP) g_swap = c.checked;
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    int s = hitSlider(x, y);
    if (s >= 0)
    {
      g_dragSlider = s;
      sliderSetFromX(s, x);
      InvalidateRect(hwnd, nullptr, FALSE);
    }
    return 0;
  }

  case WM_LBUTTONUP:
    g_dragSlider = -1;
    ReleaseCapture();
    return 0;

  case WM_MOUSEWHEEL:
  {
    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    ScreenToClient(hwnd, &pt);
    int s = hitSlider(pt.x, pt.y);
    if (s >= 0)
    {
      std::atomic<float> *p = (s == 0) ? &g_speed : (s == 1) ? &g_deadzone : &g_accel;
      float step = (s == 0) ? 0.002f : (s == 1) ? 200.f : 0.1f;
      float v = p->load() + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? step : -step);
      float mn = (s == 0) ? 0.002f : 0.f;
      float mx = (s == 0) ? 0.150f : (s == 1) ? 15000.f : 4.f;
      if (v < mn) v = mn;
      if (v > mx) v = mx;
      if (s == 2 && v < 0.15f) v = 0.f;
      *p = v;
      InvalidateRect(hwnd, nullptr, FALSE);
    }
    return 0;
  }

  case WM_KEYDOWN:
    if (wp == VK_ESCAPE && g_recTarget >= 0)
    {
      g_recTarget = -1;
      InvalidateRect(hwnd, nullptr, FALSE);
    }
    return 0;

  case WM_ERASEBKGND:
    return 1;

  case WM_PAINT:
    paint(hwnd);
    return 0;

  case WM_CLOSE:
    saveConfig();
    DestroyWindow(hwnd);
    return 0;

  case WM_DESTROY:
    KillTimer(hwnd, 1);
    g_stop = true;
    if (g_thread) { WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); }
    if (g_glowGreen.dc) { DeleteDC(g_glowGreen.dc); DeleteObject(g_glowGreen.bmp); }
    if (g_glowBlue.dc) { DeleteDC(g_glowBlue.dc); DeleteObject(g_glowBlue.bmp); }
    if (g_glowWhite.dc) { DeleteDC(g_glowWhite.dc); DeleteObject(g_glowWhite.bmp); }
    if (g_glowAmber.dc) { DeleteDC(g_glowAmber.dc); DeleteObject(g_glowAmber.bmp); }
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// ======================= 入口 =======================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow)
{
  SetProcessDPIAware();
  {
    HDC scr = GetDC(nullptr);
    g_dpi = GetDeviceCaps(scr, LOGPIXELSX);
    ReleaseDC(nullptr, scr);
  }

  loadConfig();

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = wndProc;
  wc.hInstance = hInst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"XpadMouseGui";
  wc.hbrBackground = CreateSolidBrush(C_BG);
  RegisterClassExW(&wc);

  g_hwnd = CreateWindowExW(0, L"XpadMouseGui", L"xpad-mouse 手柄鼠标",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, sc(960), sc(780),
                           nullptr, nullptr, hInst, nullptr);

  BOOL dark = TRUE;
  DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  int pref = DWMWCP_ROUND;
  DwmSetWindowAttribute(g_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));

  RECT wr;
  GetWindowRect(g_hwnd, &wr);
  int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
  SetWindowPos(g_hwnd, nullptr, (sw - (wr.right - wr.left)) / 2, (sh - (wr.bottom - wr.top)) / 2,
               0, 0, SWP_NOSIZE | SWP_NOZORDER);

  RECT crc;
  GetClientRect(g_hwnd, &crc);
  layout(crc.right, crc.bottom);

  ShowWindow(g_hwnd, nCmdShow);
  UpdateWindow(g_hwnd);

  MSG msg;
  while (GetMessage(&msg, nullptr, 0, 0))
  {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
  return (int)msg.wParam;
}
