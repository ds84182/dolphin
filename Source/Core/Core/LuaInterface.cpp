// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Thread.h"

#include "Core/LuaInterface.h"

#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/JitInterface.h"

#include "lua.hpp"

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <mutex>
#include <thread>

// We expose a "Pull-Style" API to the LuaJIT code.
  // "Pull-Style" is where Lua code calls C functions to handle events.
  // This is in contrast to "Push-Style", where C functions call Lua functions.
  // "Push-Style" in LuaJIT is slow and suboptimal, since the JIT would have to be entered and exited
    // repetitively. With the "Pull-Style" code, the Lua thread stays in the JIT compiled code, allowing for high
    // performance.
// Lua code runs in its own thread.
// Other Dolphin threads push events through the Lua interface.

namespace Lua
{

static std::thread s_lua_thread;
static std::mutex s_event_done_mutex;
static std::mutex s_event_signal_mutex;
static std::condition_variable s_event_set_cond;

static uint16_t s_event;
static std::atomic<uint32_t> s_event_mask[8];

static void LuaThread();
static void PushSymbols(lua_State *L);

static const char *Dolphin_Evaluate_Script = nullptr;

static void Dolphin_AddEventMask(uint16_t event);
static void Dolphin_RemoveEventMask(uint16_t event);

void Init()
{
  s_event = Event::INVALID;

  for (unsigned i=0; i<8; i++)
    s_event_mask[i].store(0);

  Dolphin_AddEventMask(Event::STOP);
  Dolphin_AddEventMask(Event::EVALUATE);

  s_lua_thread = std::thread(LuaThread);
}

void Shutdown()
{
  if (s_lua_thread.joinable())
  {
    Signal(Event::STOP);
    s_lua_thread.join();
  }
}

static void LuaThread()
{
  std::unique_lock<std::mutex> lock(s_event_done_mutex);

  Common::SetCurrentThreadName("Lua thread");

  int result;
  lua_State *L;

  // All Lua contexts are held in this structure. We work with it almost all the time.
  L = luaL_newstate();

  if (!L)
    return;

  // Load Lua base libraries
  luaL_openlibs(L);

  // Require the Dolphin library
  constexpr const char *boot_script =
    "local sysdir, symbols = ...;"
    "package.path = package.path..';'..sysdir..'Lua/?.lua'..';'..sysdir..'Lua/?/init.lua';"
    "_DOLPHIN_SYMS = symbols;"
    "require('dolphin').main();";

  result = luaL_loadstring(L, boot_script); // [ bootfunc: function ] || [ error: string ]

  if (result == 0) {
    // [ bootfunc: function ]
    lua_pushstring(L, File::GetSysDirectory().c_str()); // [ sysdir: string, bootfunc: function ]

    PushSymbols(L); // [ symbols: table, sysdir: string, bootfunc: function ]

    result = lua_pcall(L, 2, 0, 0); // [] || [ error: string ]
  }

  if (result != 0) {
    // [ error: string ]

    // An error occurred while trying to require the Dolphin library
    // TODO: Panic alerts should be redirected to log
    PanicAlert("Failed to run Dolphin Lua library: %s\n", lua_tostring(L, -1));
    ERROR_LOG(SCRIPT, "Failed to run Dolphin Lua library: %s\n", lua_tostring(L, -1));

    lua_pop(L, -1); // []
  }
  // []

  lua_close(L);
}

static void Dolphin_AddEventMask(uint16_t event)
{
  if (event < Event::INVALID) {
    unsigned i = 0;
    while (event >= 32) {
      i++;
      event -= 32;
    }
    s_event_mask[i] |= (1 << event);
  }
}

static void Dolphin_RemoveEventMask(uint16_t event) 
{
  if (event < Event::INVALID) {
    unsigned i = 0;
    while (event >= 32) {
      i++;
      event -= 32;
    }
    s_event_mask[i] &= ~(1 << event);
  }
}

static bool TestEvent(uint16_t event) {
  if (event < Event::INVALID) {
    unsigned i = 0;
    while (event >= 32) {
      i++;
      event -= 32;
    }
    return s_event_mask[i] & (1 << event);
  }
  return false;
}

static bool HasEvent() {
  return s_event < Event::INVALID;
}

static uint16_t Wait(uint64_t timeout_ms) {
  // The Lua thread already has the mutex locked, so adopt
  std::unique_lock<std::mutex> lock(s_event_done_mutex, std::adopt_lock);

  s_event_set_cond.wait_for(
    lock,
    std::chrono::milliseconds(timeout_ms),
    &Lua::HasEvent
  );

  uint16_t ev = s_event;
  s_event = Event::INVALID;

  // We want to keep the mutex locked while the Lua thread runs, so release (but don't unlock)
  lock.release();

  return ev;
}

void Signal(uint16_t event) {
  // Fast exit if the Lua state isn't listening to the signal
  if (!TestEvent(event)) return;

  std::unique_lock<std::mutex> siglk(s_event_signal_mutex);
  std::unique_lock<std::mutex> lk(s_event_done_mutex);

  // TODO: Compare event masks
  if (HasEvent()) {
    // Theres an event already, but the Lua thread hasn't processed it yet.
    // This _SHOULDN'T_ happen, but document it regardless.
    PanicAlert("Signal conflict!");
  }

  s_event = event;

  // Unlock so the Lua thread can lock on notify
  lk.unlock();

  // Allow the Lua thread to lock the lock
  s_event_set_cond.notify_one();

  // Relock so we can exit when the Lua thread is done
  lk.lock();
}

void Evaluate(const std::string &script) {
  Dolphin_Evaluate_Script = script.c_str();
  Signal(Event::EVALUATE);
}

static void Dolphin_Log(int level, const char *text) {
  GENERIC_LOG(LogTypes::SCRIPT, static_cast<LogTypes::LOG_LEVELS>(level), "%s", text);
}

// Push the symbols as light userdata to the Lua context
// This mitigates issues that would be hit on various platforms from regular symbol lookup
static void PushSymbols(lua_State *L) {
  lua_createtable(L, 0, 50); // [ symbols: table ]
                             // [ address: light-userdata, symbols: table ]
#define TYPE(...) __VA_ARGS__
#define BINDN(func, name, type) \
  static_assert(std::is_same_v<decltype(&func), type>, "typeof " #func " is not " #type); \
  lua_pushlightuserdata(L, &func); \
  lua_setfield(L, -2, #name); \
  lua_pushstring(L, #type); \
  lua_setfield(L, -2, "typeof_" #name)
#define BIND(x, type) BINDN(x, x, type)
  BIND(Dolphin_AddEventMask, void(*)(uint16_t event));
  BIND(Dolphin_RemoveEventMask, void(*)(uint16_t event));
  BINDN(Wait, Dolphin_Wait, uint16_t(*)(uint64_t timeout_ms));
  BIND(Dolphin_Evaluate_Script, const char **);
  BINDN(MsgAlert, Dolphin_MsgAlert, TYPE(bool(*)(bool yes_no, int Style, const char *format, ...)));
  BIND(Dolphin_Log, TYPE(void(*)(int level, const char *text)));

  BINDN(PowerPC::HostIsRAMAddress, Dolphin_Mem_IsRamAddress, bool(*)(uint32_t address));

  BINDN(PowerPC::HostRead_U8, Dolphin_Mem_Read8, uint8_t(*)(uint32_t address));
  BINDN(PowerPC::HostRead_U16, Dolphin_Mem_Read16, uint16_t(*)(uint32_t address));
  BINDN(PowerPC::HostRead_U32, Dolphin_Mem_Read32, uint32_t(*)(uint32_t address));
  BINDN(PowerPC::HostRead_U64, Dolphin_Mem_Read64, uint64_t(*)(uint32_t address));

  BINDN(PowerPC::HostWrite_U8, Dolphin_Mem_Write8, TYPE(void(*)(uint8_t value, uint32_t address)));
  BINDN(PowerPC::HostWrite_U16, Dolphin_Mem_Write16, TYPE(void(*)(uint16_t value, uint32_t address)));
  BINDN(PowerPC::HostWrite_U32, Dolphin_Mem_Write32, TYPE(void(*)(uint32_t value, uint32_t address)));
  BINDN(PowerPC::HostWrite_U64, Dolphin_Mem_Write64, TYPE(void(*)(uint64_t value, uint32_t address)));

  BINDN(
    JitInterface::InvalidateICache, 
    Dolphin_Mem_InvalidateICache, 
    TYPE(void(*)(uint32_t address, uint32_t size, bool forced))
  );
#undef BIND
#undef BINDN
}

} // Lua
