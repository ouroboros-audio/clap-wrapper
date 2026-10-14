#pragma once
#include <atomic>
#include <mutex>
#include <thread>

namespace Clap
{
// A host may change render mode while an earlier callback is still finishing.
// Only the control side waits. A callback during reconfiguration returns silence.
class ProcessGate
{
  static constexpr unsigned closed = 1u << 31;
  std::atomic<unsigned> state{0};
  std::mutex controls;
 public:
  class Reader
  {
    ProcessGate *gate;
   public:
    explicit Reader(ProcessGate &g) : gate(nullptr)
    {
      auto current = g.state.load(std::memory_order_acquire);
      while (!(current & closed))
        if (g.state.compare_exchange_weak(current, current + 1, std::memory_order_acquire)) { gate = &g; break; }
    }
    ~Reader() { if (gate) gate->state.fetch_sub(1, std::memory_order_release); }
    explicit operator bool() const { return gate != nullptr; }
    Reader(const Reader &) = delete;
  };
  class Suspension
  {
    ProcessGate &gate;
    std::unique_lock<std::mutex> lock;
   public:
    explicit Suspension(ProcessGate &g) : gate(g), lock(g.controls)
    {
      gate.state.fetch_or(closed, std::memory_order_acq_rel);
      while (gate.state.load(std::memory_order_acquire) != closed) std::this_thread::yield();
    }
    ~Suspension() { gate.state.store(0, std::memory_order_release); }
    Suspension(const Suspension &) = delete;
  };
};
}
