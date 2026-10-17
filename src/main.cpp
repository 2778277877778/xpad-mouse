/*------------------------------------------------------------------------------
  xpad-mouse — 把 Xbox 手柄摇杆映射成鼠标指针
  基于 Gopher360 (https://github.com/Tylemagne/Gopher360, GPL-3.0) 改装。

  改动（相对上游）：
    - 精简为「摇杆鼠标 + 鼠标键映射」，移除全键盘映射 / 屏幕键盘 / 音量等功能
    - 默认映射：A=左键、B=右键、X=中键
    - 修正上游 INPUT 结构体未 ZeroMemory 的问题
    - 新增：DPI 感知、多手柄自动选择、断线等待重连、
           BACK+START 长按 1 秒退出、--selftest 观测模式、--movetest 自动移动测试
    - 滚轮改为小数累加取整（上游把负 float 直接转 DWORD，属未定义行为）

  核心算法（死区归一化、亚像素累加、按键边沿检测）沿用上游实现。
-------------------------------------------------------------------------------*/
#include <windows.h>
#include <xinput.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>

#include "CXBOXController.h"
#include "ConfigFile.h"

// ======================= 全局状态 =======================
static volatile bool g_running = true;
static bool g_leftDown = false;   // 我们注入的左键是否处于按下
static bool g_rightDown = false;
static bool g_middleDown = false;
static bool g_x1Down = false;
static bool g_x2Down = false;

// 关闭窗口 / Ctrl+C 时尽量释放注入的按键，避免按键卡死
static BOOL WINAPI onConsoleEvent(DWORD type)
{
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
      type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT ||
      type == CTRL_SHUTDOWN_EVENT)
  {
    g_running = false;
    return TRUE;
  }
  return FALSE;
}

// ======================= 鼠标事件辅助 =======================
// 发送鼠标事件。上游版本未 ZeroMemory，dx/dy 等字段是垃圾值，这里修正。
static void sendMouse(DWORD flags, DWORD data = 0)
{
  INPUT in;
  ZeroMemory(&in, sizeof(in));
  in.type = INPUT_MOUSE;
  in.mi.dwFlags = flags;
  if (flags == MOUSEEVENTF_WHEEL || flags == MOUSEEVENTF_HWHEEL ||
      flags == MOUSEEVENTF_XDOWN || flags == MOUSEEVENTF_XUP)
  {
    in.mi.mouseData = data;
  }
  SendInput(1, &in, sizeof(INPUT));
}

// 释放所有注入中的鼠标键
static void releaseMouseButtons()
{
  if (g_leftDown)  { sendMouse(MOUSEEVENTF_LEFTUP);   g_leftDown = false; }
  if (g_rightDown) { sendMouse(MOUSEEVENTF_RIGHTUP);  g_rightDown = false; }
  if (g_middleDown){ sendMouse(MOUSEEVENTF_MIDDLEUP); g_middleDown = false; }
  if (g_x1Down)    { sendMouse(MOUSEEVENTF_XUP, XBUTTON1); g_x1Down = false; }
  if (g_x2Down)    { sendMouse(MOUSEEVENTF_XUP, XBUTTON2); g_x2Down = false; }
}

// ======================= 配置 =======================
struct SpeedPreset { float value; std::string name; };

struct Config
{
  DWORD mouseLeft = XINPUT_GAMEPAD_A;
  DWORD mouseRight = XINPUT_GAMEPAD_B;
  DWORD mouseMiddle = 0;   // 由 config.ini 决定，默认 X=中键
  DWORD mouseX1 = 0;       // 侧键1（网页后退）
  DWORD mouseX2 = 0;       // 侧键2（网页前进）
  DWORD speedChange = XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER;
  float deadzone = 6000.f;
  float scrollDeadzone = 5000.f;
  float scrollSpeed = 0.1f;
  float accel = 0.f;
  bool swapSticks = false;
  std::vector<SpeedPreset> speeds;
};

static void loadConfig(Config &cfg)
{
  ConfigFile file("config.ini");   // 文件不存在时 ConfigFile 会自动生成默认配置

  cfg.mouseLeft   = strtol(file.getValueOfKey<std::string>("CONFIG_MOUSE_LEFT").c_str(), 0, 0);
  cfg.mouseRight  = strtol(file.getValueOfKey<std::string>("CONFIG_MOUSE_RIGHT").c_str(), 0, 0);
  cfg.mouseMiddle = strtol(file.getValueOfKey<std::string>("CONFIG_MOUSE_MIDDLE").c_str(), 0, 0);
  cfg.mouseX1 = strtol(file.getValueOfKey<std::string>("CONFIG_MOUSE_X1").c_str(), 0, 0);
  cfg.mouseX2 = strtol(file.getValueOfKey<std::string>("CONFIG_MOUSE_X2").c_str(), 0, 0);
  cfg.speedChange = strtol(file.getValueOfKey<std::string>("CONFIG_SPEED_CHANGE").c_str(), 0, 0);

  cfg.deadzone = (float)strtol(file.getValueOfKey<std::string>("DEAD_ZONE").c_str(), 0, 0);
  if (cfg.deadzone <= 0.f) cfg.deadzone = 6000.f;

  cfg.scrollDeadzone = (float)strtol(file.getValueOfKey<std::string>("SCROLL_DEAD_ZONE").c_str(), 0, 0);
  if (cfg.scrollDeadzone <= 0.f) cfg.scrollDeadzone = 5000.f;

  cfg.scrollSpeed = strtof(file.getValueOfKey<std::string>("SCROLL_SPEED").c_str(), 0);
  if (cfg.scrollSpeed < 0.00001f) cfg.scrollSpeed = 0.1f;

  cfg.accel = strtof(file.getValueOfKey<std::string>("ACCELERATION_FACTOR").c_str(), 0);

  cfg.swapSticks = strtol(file.getValueOfKey<std::string>("SWAP_THUMBSTICKS").c_str(), 0, 0) != 0;

  // 解析速度表，如 "慢=0.005,中=0.015,快=0.025,极快=0.04"
  std::string speedsCfg = file.getValueOfKey<std::string>("CURSOR_SPEED");
  cfg.speeds.clear();
  size_t start = 0;
  int autoIdx = 1;
  while (start <= speedsCfg.size())
  {
    size_t comma = speedsCfg.find(',', start);
    std::string entry = speedsCfg.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!entry.empty())
    {
      size_t eq = entry.find('=');
      std::string name, valueStr;
      if (eq != std::string::npos)
      {
        name = entry.substr(0, eq);
        valueStr = entry.substr(eq + 1);
      }
      else
      {
        name = std::to_string(autoIdx);
        valueStr = entry;
      }
      float v = strtof(valueStr.c_str(), 0);
      if (v > 0.0001f && v <= 1.0f)
      {
        cfg.speeds.push_back({v, name});
      }
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
    autoIdx++;
  }
  if (cfg.speeds.empty())
  {
    cfg.speeds = { {0.005f, "ULTRALOW"}, {0.015f, "LOW"}, {0.025f, "MED"}, {0.04f, "HIGH"} };
  }
}

// ======================= 摇杆算法（沿用上游） =======================
// 过滤无线手柄偶发的毛刺值（上游注释：会把指针甩到屏幕边缘）
static float stickDelta(short t)
{
  if (t > 32767) t = 0;
  if (t < -32768) t = 0;
  return (float)t;
}

// 死区归一化 + 可选幂曲线，按帧率缩放
static float stickMult(float lengthsq, float deadzone, float accel, int fps)
{
  float mult = (sqrtf(lengthsq) - deadzone) / (32767.f - deadzone);
  if (mult < 0.f) mult = 0.f;
  if (accel > 0.0001f) mult = powf(mult, accel);
  return mult / (float)fps;
}

// 按键边沿检测：返回 1=刚按下 2=刚释放 0=无变化
static int buttonEdge(WORD current, WORD previous, WORD code)
{
  if (!code) return 0;
  bool down = (current & code) == code;
  bool was  = (previous & code) == code;
  if (down && !was) return 1;
  if (!down && was) return 2;
  return 0;
}

// ======================= 主程序 =======================
int main(int argc, char **argv)
{
  // UTF-8 控制台，保证中文正常显示
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  SetConsoleTitleA("xpad-mouse 手柄鼠标");

  // 高分屏下坐标 1:1（上游没有这个，缩放屏幕会偏移）
  SetProcessDPIAware();

  SetConsoleCtrlHandler(onConsoleEvent, TRUE);

  // 解析命令行
  int selftestSec = 0;   // --selftest N：只读打印手柄数据 N 秒后退出（不移动鼠标）
  int movetestSec = 0;   // --movetest N：自动画方块移动鼠标 N 秒后退出（验证注入）
  for (int i = 1; i < argc; i++)
  {
    if (strcmp(argv[i], "--selftest") == 0 && i + 1 < argc) selftestSec = atoi(argv[++i]);
    if (strcmp(argv[i], "--movetest") == 0 && i + 1 < argc) movetestSec = atoi(argv[++i]);
  }

  Config cfg;
  loadConfig(cfg);

  printf("==============================================\n");
  printf(" xpad-mouse 手柄 -> 鼠标\n");
  printf(" 基于 Gopher360 改装 (GPL-3.0)\n");
  printf("==============================================\n");
  printf(" 左摇杆  = 移动鼠标指针\n");
  if (cfg.mouseLeft)   printf(" A       = 左键\n");
  if (cfg.mouseRight)  printf(" B       = 右键\n");
  if (cfg.mouseMiddle) printf(" X       = 中键\n");
  if (cfg.mouseX1) printf(" 侧键1   = 鼠标 X1（后退）\n");
  if (cfg.mouseX2) printf(" 侧键2   = 鼠标 X2（前进）\n");
  printf(" 右摇杆  = 滚轮\n");
  if (cfg.speedChange) printf(" LB+RB   = 切换指针速度\n");
  printf(" BACK+START 按住1秒 = 退出\n");
  printf("----------------------------------------------\n");
  if (selftestSec > 0)
  {
    printf(" [自测模式] 只读打印手柄数据 %d 秒，不会移动鼠标\n", selftestSec);
  }
  else if (movetestSec > 0)
  {
    printf(" [移动测试] 将自动画方块移动鼠标 %d 秒\n", movetestSec);
  }
  else if (selftestSec == 0 && movetestSec == 0)
  {
    printf(" 正在搜索手柄...\n");
  }
  printf("\n");

  // 4 个 XInput 槽位，自动选第一个连接的手柄
  CXBOXController pads[4] = { CXBOXController(1), CXBOXController(2),
                              CXBOXController(3), CXBOXController(4) };
  int activePad = -1;

  const int FPS = 150;
  const int SLEEP_AMOUNT = 1000 / FPS;

  WORD prevButtons = 0;
  float xRest = 0.f, yRest = 0.f;       // 亚像素累加（沿用上游）
  float wheelAccV = 0.f, wheelAccH = 0.f; // 滚轮小数累加（改进项）
  size_t speedIdx = 0;
  float speed = cfg.speeds[0].value;
  int exitHoldMs = 0;
  bool waitingPrinted = false;
  int disconnectedFrames = 0;
  time_t startTime = time(NULL);

  // --movetest 的画方块轨迹：每帧走 1 像素，每边约 800ms（约 120px）
  int movePhase = 0, movePhaseMs = 0;
  const int MOVE_PHASE_MS = 800;  // 每边耗时

  while (g_running)
  {
    Sleep(SLEEP_AMOUNT);

    // 限时模式到点退出
    if (selftestSec > 0 && (int)(time(NULL) - startTime) >= selftestSec) break;
    if (movetestSec > 0 && (int)(time(NULL) - startTime) >= movetestSec) break;

    // 手柄连接管理：断线后自动重新寻找
    if (activePad >= 0 && !pads[activePad].IsConnected())
    {
      releaseMouseButtons();
      prevButtons = 0;
      activePad = -1;
    }
    if (activePad < 0)
    {
      for (int i = 0; i < 4; i++)
      {
        if (pads[i].IsConnected())
        {
          activePad = i;
          printf("[+] 已连接 XInput 手柄 #%d\n\n", activePad + 1);
          waitingPrinted = false;
          break;
        }
      }
      if (activePad < 0)
      {
        if (!waitingPrinted)
        {
          printf("[*] 未检测到手柄，请开机或插上接收器...\n");
          waitingPrinted = true;
        }
        if (++disconnectedFrames % (FPS / 2) == 0)
        {
          // 每 0.5 秒打一个点表示仍在等待
          printf(".");
          fflush(stdout);
        }
        Sleep(100);
        continue;
      }
    }

    XINPUT_STATE st = pads[activePad].GetState();
    WORD buttons = st.Gamepad.wButtons;

    // ---- 自测模式：只打印，不产生任何鼠标动作 ----
    if (selftestSec > 0)
    {
      static int printCounter = 0;
      if (++printCounter >= FPS / 2)   // 每 0.5 秒打印一行
      {
        printCounter = 0;
        printf("LX=%6d LY=%6d RX=%6d RY=%6d LT=%3u RT=%3u |",
               st.Gamepad.sThumbLX, st.Gamepad.sThumbLY,
               st.Gamepad.sThumbRX, st.Gamepad.sThumbRY,
               st.Gamepad.bLeftTrigger, st.Gamepad.bRightTrigger);
        if (buttons & XINPUT_GAMEPAD_A) printf(" A");
        if (buttons & XINPUT_GAMEPAD_B) printf(" B");
        if (buttons & XINPUT_GAMEPAD_X) printf(" X");
        if (buttons & XINPUT_GAMEPAD_Y) printf(" Y");
        if (buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) printf(" LB");
        if (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) printf(" RB");
        if (buttons & XINPUT_GAMEPAD_BACK) printf(" BACK");
        if (buttons & XINPUT_GAMEPAD_START) printf(" START");
        if (buttons & XINPUT_GAMEPAD_DPAD_UP) printf(" UP");
        if (buttons & XINPUT_GAMEPAD_DPAD_DOWN) printf(" DOWN");
        if (buttons & XINPUT_GAMEPAD_DPAD_LEFT) printf(" LEFT");
        if (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) printf(" RIGHT");
        printf("\n");
        fflush(stdout);
      }
      prevButtons = buttons;
      continue;
    }

    // ---- --movetest：自动画方块（验证鼠标注入链路） ----
    if (movetestSec > 0)
    {
      POINT cur;
      GetCursorPos(&cur);
      movePhaseMs += SLEEP_AMOUNT;
      if (movePhaseMs >= MOVE_PHASE_MS)
      {
        movePhaseMs = 0;
        movePhase = (movePhase + 1) % 4;
        if (movePhase == 0) printf("  画完一圈\n");
      }
      const int step = 1;   // 每帧 1 像素
      if (movePhase == 0) SetCursorPos(cur.x + step, cur.y);
      else if (movePhase == 1) SetCursorPos(cur.x, cur.y + step);
      else if (movePhase == 2) SetCursorPos(cur.x - step, cur.y);
      else SetCursorPos(cur.x, cur.y - step);
      prevButtons = buttons;
      continue;
    }

    // ================= 正式模式 =================

    // ---- 1. 左摇杆移动指针（算法沿用上游，亚像素累加防止低速丢步） ----
    short tx = cfg.swapSticks ? st.Gamepad.sThumbRX : st.Gamepad.sThumbLX;
    short ty = cfg.swapSticks ? st.Gamepad.sThumbRY : st.Gamepad.sThumbLY;

    POINT cursor;
    GetCursorPos(&cursor);

    float x = (float)cursor.x + xRest;
    float y = (float)cursor.y + yRest;
    float dx = 0.f, dy = 0.f;

    float lengthsq = (float)tx * tx + (float)ty * ty;
    if (lengthsq > cfg.deadzone * cfg.deadzone)
    {
      float mult = speed * stickMult(lengthsq, cfg.deadzone, cfg.accel, FPS);
      dx = stickDelta(tx) * mult;
      dy = stickDelta(ty) * mult;
    }

    x += dx;
    y -= dy;                      // 摇杆向上（ty 为负）= 鼠标向上（y 减小）
    xRest = x - (float)((int)x);
    yRest = y - (float)((int)y);

    if (dx != 0.f || dy != 0.f)
    {
      SetCursorPos((int)x, (int)y);
    }

    // ---- 2. 右摇杆滚轮（小数累加取整，正负都正确） ----
    short sx = cfg.swapSticks ? st.Gamepad.sThumbLX : st.Gamepad.sThumbRX;
    short sy = cfg.swapSticks ? st.Gamepad.sThumbLY : st.Gamepad.sThumbRY;
    float scrollLenSq = (float)sx * sx + (float)sy * sy;
    if (scrollLenSq > cfg.scrollDeadzone * cfg.scrollDeadzone)
    {
      float mult = stickMult(scrollLenSq, cfg.scrollDeadzone, 0.f, FPS) * cfg.scrollSpeed;
      wheelAccV += (float)sy * mult;
      wheelAccH += (float)sx * mult;
      if (wheelAccV >= 1.f || wheelAccV <= -1.f)
      {
        int notches = (int)wheelAccV;
        wheelAccV -= (float)notches;
        sendMouse(MOUSEEVENTF_WHEEL, (DWORD)(int)notches);
      }
      if (wheelAccH >= 1.f || wheelAccH <= -1.f)
      {
        int notches = (int)wheelAccH;
        wheelAccH -= (float)notches;
        sendMouse(MOUSEEVENTF_HWHEEL, (DWORD)(int)notches);
      }
    }
    else
    {
      wheelAccV = 0.f;
      wheelAccH = 0.f;
    }

    // ---- 3. 鼠标键映射：按下沿注入 down，释放沿注入 up ----
    if (cfg.mouseLeft && buttonEdge(buttons, prevButtons, (WORD)cfg.mouseLeft))
    {
      bool down = ((buttons & cfg.mouseLeft) == cfg.mouseLeft);
      sendMouse(down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
      g_leftDown = down;
    }
    if (cfg.mouseRight && buttonEdge(buttons, prevButtons, (WORD)cfg.mouseRight))
    {
      bool down = ((buttons & cfg.mouseRight) == cfg.mouseRight);
      sendMouse(down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
      g_rightDown = down;
    }
    if (cfg.mouseMiddle && buttonEdge(buttons, prevButtons, (WORD)cfg.mouseMiddle))
    {
      bool down = ((buttons & cfg.mouseMiddle) == cfg.mouseMiddle);
      sendMouse(down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP);
      g_middleDown = down;
    }
    if (cfg.mouseX1 && buttonEdge(buttons, prevButtons, (WORD)cfg.mouseX1))
    {
      bool down = ((buttons & cfg.mouseX1) == cfg.mouseX1);
      sendMouse(down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, XBUTTON1);
      g_x1Down = down;
    }
    if (cfg.mouseX2 && buttonEdge(buttons, prevButtons, (WORD)cfg.mouseX2))
    {
      bool down = ((buttons & cfg.mouseX2) == cfg.mouseX2);
      sendMouse(down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, XBUTTON2);
      g_x2Down = down;
    }

    // ---- 4. LB+RB 切换速度（带震动提示） ----
    if (cfg.speedChange && buttonEdge(buttons, prevButtons, (WORD)cfg.speedChange) == 1)
    {
      speedIdx = (speedIdx + 1) % cfg.speeds.size();
      speed = cfg.speeds[speedIdx].value;
      printf("[速度] 当前档位：%s (%.3f)\n", cfg.speeds[speedIdx].name.c_str(), speed);
      fflush(stdout);
      pads[activePad].Vibrate(40000, 40000);
      Sleep(80);
      pads[activePad].Vibrate(0, 0);
    }

    // ---- 5. BACK+START 按住 1 秒退出 ----
    if ((buttons & (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START)) ==
        (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START))
    {
      exitHoldMs += SLEEP_AMOUNT;
      if (exitHoldMs >= 1000)
      {
        printf("\n[退出] BACK+START 长按 1 秒，程序退出。\n");
        g_running = false;
      }
    }
    else
    {
      exitHoldMs = 0;
    }

    prevButtons = buttons;
  }

  // 退出前释放注入的按键，防止鼠标键卡住
  releaseMouseButtons();
  printf("\n已退出。\n");
  return 0;
}
