/*------------------------------------------------------------------------------
  xpad-mouse — 手柄摇杆映射鼠标
  本文件来自 Gopher360 (https://github.com/Tylemagne/Gopher360)，未做逻辑修改。
  Gopher360 以 GNU GPL v3 发布，见上级目录 LICENSE。
-------------------------------------------------------------------------------*/
#pragma once

#include <windows.h>
#include <xinput.h>

class CXBOXController
{
private:
  XINPUT_STATE _controllerState;
  int _controllerNum;
public:
  CXBOXController(int playerNumber);
  XINPUT_STATE GetState();
  bool IsConnected();
  void Vibrate(int leftVal, int rightVal);
};
