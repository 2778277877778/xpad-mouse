# xpad-mouse — Xbox 手柄摇杆映射鼠标

把 Xbox 手柄（有线 / 2.4G 无线接收器 / Xbox One / 兼容 XInput 的手柄）变成鼠标：
**左摇杆移动指针，A = 左键，B = 右键**。

两个版本，共用一份配置：

- `xpad-mouse.exe` — **图形界面版**（推荐）：深色界面，灵敏度/死区/加速曲线实时可调，带摇杆可视化
- `xpad-mouse-cli.exe` — 控制台版：零界面，双击即用，适合开机自启

均为单文件 exe，静态编译无依赖。

## 图形界面版功能

- **指针灵敏度滑条**（0.002 ~ 0.150），拖动或滚轮微调，实时生效不用重启
- **摇杆死区滑条**：指针自己漂移就调大；预览图里的虚线圆圈就是当前死区范围
- **加速曲线滑条**：0 = 线性；填 2~3 = 慢速更精细、快速更迅猛
- **自定义按键映射**：左键/右键/中键/侧键1/侧键2 五个目标，点击芯片后按手柄任意键
  （A/B/X/Y、LB/RB、LT/RT 扳机、十字键、BACK/START）即可绑定；支持先松开再按、
  ESC 或再点一次取消；「恢复默认」一键还原 A=左键、B=右键、X=中键
- **摇杆实时预览**：绿点 = 左摇杆，蓝点 = 右摇杆（带拖尾动画），直观确认回中
- **按键指示灯**：A/B/X/Y/LB/RB/LT/RT/BACK/START 共 10 个灯，按下亮起带余辉
- **本窗口聚焦时暂停映射**（默认开）：调参数时手柄不会抢鼠标；切到别的窗口后映射恢复
- LB+RB 循环切换速度档（震动提示）；BACK+START 按住 1 秒退出
- 所有设置自动保存到 `config.ini`，下次启动直接生效

## 按键映射

| 手柄输入 | 鼠标功能（均可在 GUI 中改绑） |
|---------|---------|
| 左摇杆 | 移动指针 |
| A | 左键（按住可拖动） |
| B | 右键 |
| X | 中键 |
| 侧键1 / 侧键2 | 默认未绑定（可在 GUI 绑定为鼠标侧键，网页后退/前进） |
| 右摇杆 | 滚轮（上下 + 左右横滚） |
| LB + RB 同按 | 切换指针速度档 |
| BACK + START 按住 1 秒 | 退出 |

`config.ini` 中 `0x10000` = LT、`0x20000` = RT（扳机），可与普通键码相加组成组合键。
控制台版不识别扳机位（只认 16 位键码），扳机绑定仅 GUI 版生效。

## 配置文件

`config.ini` 与 exe 同目录（UTF-8），GUI 版保存/读取与 CLI 版完全兼容。
删除后自动按默认值运行；GUI 里点「恢复默认」可一键重置。

## 编译（开发者）

需要 MinGW-w64（g++）：

```bat
:: 图形界面版
g++ -O2 -std=c++17 -static -mwindows -o build/xpad-mouse.exe ^
    src/gui.cpp src/CXBOXController.cpp src/ConfigFile.cpp ^
    -lxinput1_4 -luser32 -lgdi32 -ldwmapi

:: 控制台版
g++ -O2 -std=c++17 -static -o build/xpad-mouse-cli.exe ^
    src/main.cpp src/CXBOXController.cpp src/ConfigFile.cpp ^
    -lxinput1_4 -luser32
```

## 来源与许可

基于开源项目 [Gopher360](https://github.com/Tylemagne/Gopher360)（作者 Tylemagne）改装，
以 **GNU GPL v3** 发布（见 `LICENSE`）。

保留自上游：`CXBOXController`（XInput 封装）、`ConfigFile`（INI 解析）、
核心算法（死区归一化、亚像素累加、按键边沿检测）。

改动：

- 精简为「摇杆鼠标 + 鼠标键映射」，移除全键盘映射 / 屏幕键盘 / 音量等功能
- 新增自绘深色 GUI（灵敏度/死区/加速曲线实时可调、摇杆可视化、按键指示灯）
- 默认灵敏度提高到 0.03（上游默认 0.015）
- 修正上游 `INPUT` 结构体未 `ZeroMemory` 的问题
- 新增 DPI 感知、多手柄自动选择、断线等待重连、BACK+START 长按退出
- 滚轮改为小数累加取整（上游把负 float 直接转 DWORD，属未定义行为）
