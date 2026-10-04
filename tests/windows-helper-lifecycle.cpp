#ifndef NOMINMAX
#define NOMINMAX
#endif
#define main originalNotificationFixtureMain
#include "vst3-host-notifications.cpp"
#undef main
#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>

#include "../src/detail/os/windows.cpp"

struct NativeHost : Vst::IHostApplication
{
  uint32 references = 1;
  tresult PLUGIN_API queryInterface(const TUID iid, void **object) override
  {
    *object = nullptr;
    if (std::memcmp(iid, Vst::IHostApplication::iid.toTUID(), sizeof(TUID)) != 0 &&
        std::memcmp(iid, FUnknown::iid.toTUID(), sizeof(TUID)) != 0) return kNoInterface;
    *object = this; addRef(); return kResultOk;
  }
  uint32 PLUGIN_API addRef() override { return ++references; }
  uint32 PLUGIN_API release() override { return --references; }
  tresult PLUGIN_API getName(Vst::String128 name) override
  {
    name[0] = 'P'; name[1] = 0; return kResultOk;
  }
  tresult PLUGIN_API createInstance(TUID, TUID, void **object) override
  {
    *object = nullptr; return kNoInterface;
  }
};

static std::vector<int> visitOrder;
static std::array<unsigned, 3> visitCounts{};
struct CountingWrapper : ClapAsVst3
{
  int identity;
  CountingWrapper(Clap::Library &library, int id) : ClapAsVst3(&library, 0, nullptr), identity(id) {}
  void onIdle() override
  {
    ++visitCounts[identity - 1];
    visitOrder.push_back(identity);
    ClapAsVst3::onIdle();
  }
};

struct RestartHandler : Handler
{
  std::function<void()> action;
  tresult PLUGIN_API restartComponent(int32 flags) override
  {
    Handler::restartComponent(flags);
    if (action)
    {
      auto restart = std::exchange(action, {});
      restart();
    }
    return kResultOk;
  }
};

static void require(bool condition, const char *message)
{
  if (!condition) throw std::runtime_error(message);
}

static void tick(HWND window)
{
  const auto deadline = GetTickCount64() + 1000;
  MSG message{};
  while (GetTickCount64() < deadline)
  {
    if (PeekMessageW(&message, window, WM_TIMER, WM_TIMER, PM_REMOVE))
    {
      DispatchMessageW(&message);
      return;
    }
    Sleep(1);
  }
  throw std::runtime_error("Actual WindowsHelper WM_TIMER did not arrive");
}

static void prepare(CountingWrapper &wrapper, NativeHost &host, RestartHandler &handler, bool active)
{
  require(wrapper.initialize(&host) == kResultOk, "Wrapper initialize failed");
  require(wrapper.setComponentHandler(&handler) == kResultOk, "Handler install failed");
  Vst::ProcessSetup setup{Vst::kRealtime, Vst::kSample32, 256, 48000};
  require(wrapper.setupProcessing(setup) == kResultOk, "Setup processing failed");
  if (active) require(wrapper.setActive(true) == kResultOk, "Wrapper activation failed");
}

static void run(bool grow, bool removePeer)
{
  ghInst = GetModuleHandleW(nullptr); // Native executable's module identity for the SDK helper.
  os::init();
  const auto window = FindWindowExW(HWND_MESSAGE, nullptr, os::helperClassName(ghInst).c_str(), nullptr);
  require(window != nullptr, "Actual helper native window was not created");
  Clap::Library library;
  library._pluginFactory = &factory;
  library.plugins.push_back(&descriptor);
  NativeHost host;
  RestartHandler firstHandler, survivorHandler, addedHandler;
  auto firstStorage = std::make_unique<CountingWrapper>(library, 1);
  auto survivorStorage = std::make_unique<CountingWrapper>(library, 2);
  auto addedStorage = std::make_unique<CountingWrapper>(library, 3);
  auto &first = *firstStorage;
  auto &survivor = *survivorStorage;
  auto &added = *addedStorage;
  prepare(first, host, firstHandler, true);
  prepare(survivor, host, survivorHandler, true);
  if (grow) prepare(added, host, addedHandler, false);
  tick(window);
  visitOrder.clear();
  visitCounts.fill(0);
  firstHandler.notifications.clear(); survivorHandler.notifications.clear();
  bool lifecycleOk = true;
  firstHandler.action = [&] {
    lifecycleOk &= first.setActive(false) == kResultOk;
    lifecycleOk &= first.setActive(true) == kResultOk;
    if (grow) lifecycleOk &= added.setActive(true) == kResultOk;
    if (removePeer)
    {
      lifecycleOk &= survivor.setActive(false) == kResultOk;
      survivor.setComponentHandler(nullptr);
      lifecycleOk &= survivor.terminate() == kResultOk;
      survivorStorage.reset();
    }
    std::cout << "inside restart: ordinary deactivate/reactivate succeeded=" << lifecycleOk
              << " setup-complete third activated=" << grow << " peer removed=" << removePeer << '\n' << std::flush;
  };
  first.tail_changed();
  survivor.tail_changed();
  std::cout << "before native tick: two active wrappers; third joining=" << grow << '\n' << std::flush;
  tick(window);
  std::cout << "first tick order=";
  for (const auto id : visitOrder) std::cout << id << ',';
  std::cout << " visits first=" << visitCounts[0] << " survivor=" << visitCounts[1] << " added=" << visitCounts[2]
            << " survivorNotifications=" << survivorHandler.notifications.size() << '\n' << std::flush;
  require(lifecycleOk, "The synchronous lifecycle calls failed");
  require(visitCounts[0] == 1 && visitCounts[1] == (removePeer ? 0u : 1u) && visitCounts[2] == 0,
          "Native tick duplicated the restarted instance or skipped its survivor");
  require(survivorHandler.notifications.size() == (removePeer ? 0u : 1u),
          "Native tick skipped the surviving instance notification");
  visitOrder.clear();
  tick(window);
  std::cout << "second tick order=";
  for (const auto id : visitOrder) std::cout << id << ',';
  std::cout << " survivorNotifications=" << survivorHandler.notifications.size() << '\n' << std::flush;
  require(survivorHandler.notifications.size() == (removePeer ? 0u : 1u), "Surviving instance notification was lost");
  require(visitCounts[2] == (grow ? 1u : 0u), "New instance was not serviced on the next native tick");
  first.setActive(false);
  if (!removePeer) survivor.setActive(false);
  if (grow) added.setActive(false);
  first.setComponentHandler(nullptr);
  if (!removePeer) survivor.setComponentHandler(nullptr);
  first.terminate();
  if (!removePeer) survivor.terminate();
  if (grow) { added.setComponentHandler(nullptr); added.terminate(); }
  os::terminate();
  require(!IsWindow(window), "Native helper window survived termination");
  std::cout << "Native helper probe completed\n";
}

int main(int argc, char **argv)
{
  std::promise<void> finished;
  auto done = finished.get_future();
  std::thread watchdog([&] {
    if (done.wait_for(std::chrono::seconds(15)) != std::future_status::ready) std::_Exit(3);
  });
  int result = 0;
  try
  {
    require(argc == 2 && (std::strcmp(argv[1], "ordinary") == 0 || std::strcmp(argv[1], "grow") == 0
                          || std::strcmp(argv[1], "remove") == 0), "Expected ordinary, grow or remove");
    run(std::strcmp(argv[1], "grow") == 0, std::strcmp(argv[1], "remove") == 0);
  }
  catch (const std::exception &error) { std::cerr << error.what() << '\n'; result = 1; }
  finished.set_value(); watchdog.join(); return result;
}

