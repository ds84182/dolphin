// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/MsgHandler.h"

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

API_EXPORT volatile uint64_t Dolphin_Event_Mask[4] = { 0 };
API_EXPORT volatile const char *Dolphin_Evaluate_Script = nullptr;

namespace Lua
{

static std::thread s_lua_thread;
static std::mutex s_event_done_mutex;
static std::mutex s_event_signal_mutex;
static std::condition_variable s_event_set_cond;

static uint16_t s_event;
static std::atomic<uint32_t> s_event_mask[8];

static void LuaThread();

API_EXPORT void Dolphin_AddEventMask(uint16_t event);
API_EXPORT void Dolphin_RemoveEventMask(uint16_t event);

void Init()
{
  s_event = Event::INVALID;

  for (unsigned i=0; i<8; i++)
    s_event_mask[i].store(0);

  Dolphin_AddEventMask(Event::STOP);
  Dolphin_AddEventMask(Event::EVALUATE);

  s_lua_thread = std::thread(LuaThread);

  Evaluate("dolphin.alert('Hello from Eval!')");
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
    "local sysdir = ...;"
    "package.path = package.path..';'..sysdir..'Lua/?.lua'..';'..sysdir..'Lua/?/init.lua';"
    "require('dolphin').main();";

  result = luaL_loadstring(L, boot_script); // [ bootfunc: function ] || [ error: string ]

  if (result == 0) {
    // [ bootfunc: function ]
    lua_pushstring(L, File::GetSysDirectory().c_str()); // [ sysdir: string, bootfunc: function ]
    result = lua_pcall(L, 1, 0, 0); // [] || [ error: string ]
  }

  if (result != 0) {
    // [ error: string ]

    // An error occurred while trying to require the Dolphin library
    PanicAlert("Failed to run Dolphin Lua library: %s\n", lua_tostring(L, -1));

    lua_pop(L, -1); // []
  }
  // []

  lua_close(L);
}

API_EXPORT void Dolphin_AddEventMask(uint16_t event)
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

API_EXPORT void Dolphin_RemoveEventMask(uint16_t event) 
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

static uint16_t Wait(uint16_t timeout_ms) {
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

} // Lua

API_EXPORT uint16_t Dolphin_Wait(uint16_t timeout_ms) {
  return Lua::Wait(timeout_ms);
}

// Dolphin API Exports

API_EXPORT bool(*Dolphin_MsgAlert)(bool yes_no, int Style, const char *format, ...) = MsgAlert;

API_EXPORT bool(*Dolphin_Mem_IsRamAddress)(u32 address) = PowerPC::HostIsRAMAddress;

API_EXPORT u8(*Dolphin_Mem_Read8)(u32 address) = PowerPC::HostRead_U8;
API_EXPORT u16(*Dolphin_Mem_Read16)(u32 address) = PowerPC::HostRead_U16;
API_EXPORT u32(*Dolphin_Mem_Read32)(u32 address) = PowerPC::HostRead_U32;
API_EXPORT u64(*Dolphin_Mem_Read64)(u32 address) = PowerPC::HostRead_U64;

API_EXPORT void(*Dolphin_Mem_Write8)(u8 value, u32 address) = PowerPC::HostWrite_U8;
API_EXPORT void(*Dolphin_Mem_Write16)(u16 value, u32 address) = PowerPC::HostWrite_U16;
API_EXPORT void(*Dolphin_Mem_Write32)(u32 value, u32 address) = PowerPC::HostWrite_U32;
API_EXPORT void(*Dolphin_Mem_Write64)(u64 value, u32 address) = PowerPC::HostWrite_U64;

API_EXPORT void (*Dolphin_Mem_InvalidateICache)(u32 address, u32 size, bool forced) = JitInterface::InvalidateICache;
