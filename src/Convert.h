/*------------------------------------------------------------------------------
  xpad-mouse — 手柄摇杆映射鼠标
  本文件来自 Gopher360 (https://github.com/Tylemagne/Gopher360)，未做逻辑修改。
  Gopher360 以 GNU GPL v3 发布，见上级目录 LICENSE。
-------------------------------------------------------------------------------*/
#pragma once

#include <iostream>
#include <string>
#include <sstream>
#include <map>
#include <fstream>

class Convert
{
public:
  template <typename T>
  static std::string T_to_string(T const &val)
  {
    std::ostringstream ostr;
    ostr << val;

    return ostr.str();
  };

  template <typename T>
  static T string_to_T(std::string const &val)
  {
    std::istringstream istr(val);
    T returnVal;
    if (!(istr >> returnVal))
    {
      std::cout << "CFG: Not a valid " + (std::string)typeid(T).name() + " received!\n";
      std::cin.ignore();
      std::cin.get();
      exit(EXIT_FAILURE);
    }

    return returnVal;
  };
};
