// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#ifdef _WIN32
#define API_EXPORT extern "C" __declspec(dllexport)
#else
#define API_EXPORT extern "C" __attribute__ ((visibility ("default")))
#endif

#include <variant>

namespace Lua
{
  void Init();
  void Shutdown();

  namespace Event
  {
    template <typename T>
    struct ID_t;

    template <typename T>
    constexpr uint16_t ID = ID_t<T>::value;

    struct Base {};
    struct Stop : Base {};
    struct Evaluate : Base {
      std::string script;
      Evaluate(const std::string &script) : script(script) {}
    };
    struct Frame : Base {};

    using None = std::monostate;

    template <>
    struct ID_t<None> { static constexpr uint16_t value = 256; };

    template <>
    struct ID_t<Stop> { static constexpr uint16_t value = 0; };

    template <>
    struct ID_t<Evaluate> { static constexpr uint16_t value = 1; };

    template <>
    struct ID_t<Frame> { static constexpr uint16_t value = 2; };
  }

  using AnyEvent = std::variant<Event::None, Event::Stop, Event::Evaluate, Event::Frame>;

  namespace Detail {
    bool IsEventEnabledByID(uint16_t id);
    void SignalEvent(AnyEvent &&event);
  }

  template <typename T>
  bool IsEventEnabled() {
    return Detail::IsEventEnabledByID(Event::ID<T>);
  }

  template <typename T, typename F>
  void SignalEventLazy(const F &makeEvent) {
    if (IsEventEnabled<T>()) {
      AnyEvent event = makeEvent();
      Detail::SignalEvent(std::move(event));
    }
  }

  static inline void SignalEvent(AnyEvent &&event) {
    Detail::SignalEvent(std::move(event));
  }

  static inline void Evaluate(const std::string &script) {
    SignalEventLazy<Event::Evaluate>([&]() {
      return Event::Evaluate(script);
    });
  }

  static inline void PostFrame() {
    // SignalEventLazy<Event::Frame>([]() { return Event::Frame(); });
  }
}
