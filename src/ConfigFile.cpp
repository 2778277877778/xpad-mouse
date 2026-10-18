/*------------------------------------------------------------------------------
  xpad-mouse — 手柄摇杆映射鼠标
  本文件改自 Gopher360 (https://github.com/Tylemagne/Gopher360) 的 ConfigFile。
  改动：默认配置模板替换为 xpad-mouse 的映射；写默认配置时使用 fName
  而非硬编码文件名。其余解析逻辑未改。
  Gopher360 以 GNU GPL v3 发布，见上级目录 LICENSE。
-------------------------------------------------------------------------------*/
#include "ConfigFile.h"
#include <iostream>
#include <fstream> 
#include <windows.h>

void ConfigFile::removeComment(std::string &line) const
{
  if (line.find('#') != line.npos)
  {
    line.erase(line.find('#'));
  }
}

bool ConfigFile::onlyWhitespace(const std::string &line) const
{
  return (line.find_first_not_of(' ') == line.npos);
}

bool ConfigFile::validLine(const std::string &line) const
{
  std::string temp = line;
  temp.erase(0, temp.find_first_not_of("\t "));
  if (temp[0] == '=')
  {
    return false;
  }

  for (size_t i = temp.find('=') + 1; i < temp.length(); i++)
  {
    if (temp[i] != ' ')
    {
      return true;
    }
  }

  return false;
}

void ConfigFile::extractKey(std::string &key, size_t const &sepPos, const std::string &line) const
{
  key = line.substr(0, sepPos);
  if (key.find('\t') != line.npos || key.find(' ') != line.npos)
  {
    key.erase(key.find_first_of("\t "));
  }
}
void ConfigFile::extractValue(std::string &value, size_t const &sepPos, const std::string &line) const
{
  value = line.substr(sepPos + 1);
  value.erase(0, value.find_first_not_of("\t "));
  value.erase(value.find_last_not_of("\t ") + 1);
}

void ConfigFile::extractContents(const std::string &line)
{
  std::string temp = line;
  temp.erase(0, temp.find_first_not_of("\t "));
  size_t sepPos = temp.find('=');

  std::string key, value;
  extractKey(key, sepPos, temp);
  extractValue(value, sepPos, temp);

  if (!keyExists(key))
  {
    contents.insert(std::pair<std::string, std::string>(key, value));
  }
  else
  {
    exitWithError("CFG: Can only have unique key names!\n");
  }
}

void ConfigFile::parseLine(const std::string &line, size_t const lineNo)
{
  if (line.find('=') == line.npos)
  {
    exitWithError("CFG: Couldn't find separator on line: " + Convert::T_to_string(lineNo) + "\n");
  }

  if (!validLine(line))
  {
    exitWithError("CFG: Bad format for line: " + Convert::T_to_string(lineNo) + "\n");
  }

  extractContents(line);
}

void ConfigFile::ExtractKeys()
{
  std::ifstream file;
  file.open(fName.c_str());

  if (!file)
  {
    printf("%s not found! Building a fresh one... ", fName.c_str());

    std::ofstream outfile(fName.c_str());

    // Begin config dump to file (xpad-mouse defaults)
    outfile << "# ============================================================" << std::endl;
    outfile << "#  xpad-mouse v2.4 默认配置（基于 Gopher360 配置格式，GPL-3.0）" << std::endl;
    outfile << "#  删除本文件后重新启动程序会自动生成一份新的。" << std::endl;
    outfile << "# ============================================================" << std::endl;
    outfile << "#" << std::endl;
    outfile << "# 按键码（XInput 按钮，可相加组合，如 0x0030 = BACK+START）：" << std::endl;
    outfile << "#   A=0x1000  B=0x2000  X=0x4000  Y=0x8000" << std::endl;
    outfile << "#   LB=0x0100 RB=0x0200 BACK=0x0020 START=0x0010" << std::endl;
    outfile << "#   设为 0 = 禁用该功能" << std::endl;
    outfile << std::endl;
    outfile << "# ---------- 鼠标按键映射 ----------" << std::endl;
    outfile << "CONFIG_MOUSE_LEFT = 0x1000      # 鼠标左键（默认 A）" << std::endl;
    outfile << "CONFIG_MOUSE_RIGHT = 0x2000     # 鼠标右键（默认 B）" << std::endl;
    outfile << "CONFIG_MOUSE_MIDDLE = 0x4000    # 鼠标中键（默认 X，0=禁用）" << std::endl;
    outfile << "CONFIG_MOUSE_X1 = 0             # 侧键1 网页后退（0=禁用）" << std::endl;
    outfile << "CONFIG_MOUSE_X2 = 0             # 侧键2 网页前进（0=禁用）" << std::endl;
    outfile << std::endl;
    outfile << "# ---------- 功能键 ----------" << std::endl;
    outfile << "CONFIG_SPEED_CHANGE = 0x0300    # LB+RB 同按 = 循环切换指针速度" << std::endl;
    outfile << "# 退出为内置功能：BACK+START 按住 1 秒退出程序" << std::endl;
    outfile << std::endl;
    outfile << "# ---------- 高级设置 ----------" << std::endl;
    outfile << "# 指针速度档位（第一档为默认档），LB+RB 循环切换，最大1.0" << std::endl;
    outfile << "CURSOR_SPEED = 慢=0.005,中=0.015,快=0.025,极快=0.04" << std::endl;
    outfile << "# 摇杆死区 0~32767：过小会自己漂移，过大则迟钝" << std::endl;
    outfile << "DEAD_ZONE = 6000" << std::endl;
    outfile << "# 滚轮死区" << std::endl;
    outfile << "SCROLL_DEAD_ZONE = 5000" << std::endl;
    outfile << "# 滚轮速度" << std::endl;
    outfile << "SCROLL_SPEED = 0.1" << std::endl;
    outfile << "# 渐进加速：0=关（纯线性）；1~4 推得越深、按得越久，指针提速越猛" << std::endl;
    outfile << "ACCELERATION_FACTOR = 0" << std::endl;
    outfile << "# 1 = 交换摇杆（右摇杆移鼠标，左摇杆滚轮）" << std::endl;
    outfile << "SWAP_THUMBSTICKS = 0" << std::endl;
    // End config dump

    outfile.close();

    file.open(fName.c_str());

    if (!file)
    {
      exitWithError("\nERROR! Configuration file " + fName + " still couldn't be found!\n");
    }
    else
    {
      printf("Success!\nNow using %s.\n", fName.c_str());
    }
  }

  std::string line;
  size_t lineNo = 0;
  while (std::getline(file, line))
  {
    lineNo++;
    std::string temp = line;

    if (temp.empty())
    {
      continue;
    }

    removeComment(temp);
    if (onlyWhitespace(temp))
    {
      continue;
    }

    parseLine(temp, lineNo);
  }

  file.close();
}

ConfigFile::ConfigFile(const std::string &fName)
{
  this->fName = fName;
  ExtractKeys();
}

bool ConfigFile::keyExists(const std::string &key) const
{
  return contents.find(key) != contents.end();
}

void ConfigFile::exitWithError(const std::string &error)
{
  std::cout << error;
  std::cin.ignore();
  std::cin.get();

  exit(EXIT_FAILURE);
}
