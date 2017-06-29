// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#ifdef _WIN32
#define API_EXPORT extern "C" __declspec(dllexport)
#else
#define API_EXPORT extern "C" __attribute__ ((visibility ("default")))
#endif

namespace Lua
{
  void Init();
  void Shutdown();
  void Signal(uint16_t event);
  void Evaluate(const std::string &script);

  namespace Event
  {
    constexpr uint16_t STOP = 0;
    constexpr uint16_t EVALUATE = 1;
    constexpr uint16_t FRAME = 2;
    constexpr uint16_t INVALID = 256;
  }
}
