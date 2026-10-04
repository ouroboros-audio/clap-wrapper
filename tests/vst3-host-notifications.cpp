#include "wrapasvst3.h"
#include "detail/clap/fsutil.h"
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

struct Handler : Vst::IComponentHandler
{
  uint32 references = 1;
  std::vector<int32> notifications;
  bool inActivation = false;
  bool reenteredActivation = false;
  const std::thread::id uiThread = std::this_thread::get_id();
  bool wrongThread = false;
  std::vector<Vst::ParamID> starts, edits, ends;

  tresult PLUGIN_API queryInterface(const TUID iid, void **object) override
  {
    *object = nullptr;
    if (std::memcmp(iid, Vst::IComponentHandler::iid.toTUID(), sizeof(TUID)) != 0)
      return kNoInterface;
    *object = this;
    addRef();
    return kResultOk;
  }

  uint32 PLUGIN_API addRef() override { return ++references; }
  uint32 PLUGIN_API release() override { return --references; }
  tresult PLUGIN_API beginEdit(Vst::ParamID id) override { starts.push_back(id); return kResultOk; }
  tresult PLUGIN_API performEdit(Vst::ParamID id, Vst::ParamValue) override { edits.push_back(id); return kResultOk; }
  tresult PLUGIN_API endEdit(Vst::ParamID id) override { ends.push_back(id); return kResultOk; }
  tresult PLUGIN_API restartComponent(int32 flags) override
  {
    wrongThread |= std::this_thread::get_id() != uiThread;
    reenteredActivation |= inActivation;
    notifications.push_back(flags);
    return kResultOk;
  }
};

struct HostContext : FUnknown
{
  tresult PLUGIN_API queryInterface(const TUID, void **object) override
  {
    *object = nullptr;
    return kNoInterface;
  }
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
};

struct TestPlugin
{
  const clap_host_t *host = nullptr;
  const char *title = "Initial";
  clap_id parameterId = 7;
  clap_plugin_t plugin{};

  static TestPlugin &self(const clap_plugin_t *p)
  {
    return *static_cast<TestPlugin *>(p->plugin_data);
  }

  void notify(bool activating)
  {
    const auto *params = static_cast<const clap_host_params_t *>(host->get_extension(host, CLAP_EXT_PARAMS));
    if (!activating) params->rescan(host, CLAP_PARAM_RESCAN_ALL);
    params->rescan(host, CLAP_PARAM_RESCAN_INFO);
    if (activating)
      static_cast<const clap_host_latency_t *>(host->get_extension(host, CLAP_EXT_LATENCY))->changed(host);
  }
};

static const clap_plugin_descriptor_t descriptor{CLAP_VERSION, "org.clap-wrapper.notifications",
  "Notifications", "CLAP Wrapper", "", "", "", "1", "", nullptr};

static bool replaceParameterOnDeactivate = false;

static const clap_plugin_params_t params{
  [](const clap_plugin_t *) -> uint32_t { return 1; },
  [](const clap_plugin_t *p, uint32_t i, clap_param_info_t *info) -> bool
  {
    if (i != 0) return false;
    *info = {};
    info->id = TestPlugin::self(p).parameterId;
    info->max_value = 1;
    std::strncpy(info->name, TestPlugin::self(p).title, sizeof(info->name) - 1);
    return true;
  },
  [](const clap_plugin_t *p, clap_id id, double *value) -> bool
  {
    if (id != TestPlugin::self(p).parameterId) return false;
    *value = 0.5;
    return true;
  },
  [](const clap_plugin_t *, clap_id, double, char *, uint32_t) -> bool { return false; },
  [](const clap_plugin_t *, clap_id, const char *, double *) -> bool { return false; },
  [](const clap_plugin_t *, const clap_input_events_t *, const clap_output_events_t *) {}};

static const clap_plugin_factory_t factory{
  [](const clap_plugin_factory_t *) -> uint32_t { return 1; },
  [](const clap_plugin_factory_t *, uint32_t i) -> const clap_plugin_descriptor_t *
  { return i == 0 ? &descriptor : nullptr; },
  [](const clap_plugin_factory_t *, const clap_host_t *host, const char *) -> const clap_plugin_t *
  {
    auto *p = new TestPlugin;
    p->host = host;
    p->plugin = {&descriptor, p,
      [](const clap_plugin_t *) -> bool { return true; },
      [](const clap_plugin_t *plugin) { delete &TestPlugin::self(plugin); },
      [](const clap_plugin_t *plugin, double, uint32_t, uint32_t) -> bool
      {
        auto &p = TestPlugin::self(plugin);
        p.title = "Activated";
        p.notify(true);
        return true;
      },
      [](const clap_plugin_t *plugin)
      {
        auto &p = TestPlugin::self(plugin);
        p.title = "Deactivated";
        if (replaceParameterOnDeactivate)
        {
          const auto *hostParams = static_cast<const clap_host_params_t *>(p.host->get_extension(p.host, CLAP_EXT_PARAMS));
          hostParams->clear(p.host, p.parameterId, CLAP_PARAM_CLEAR_ALL);
          p.parameterId = 8;
        }
        p.notify(false);
      },
      [](const clap_plugin_t *) -> bool { return true; },
      [](const clap_plugin_t *) {},
      [](const clap_plugin_t *) {},
      [](const clap_plugin_t *, const clap_process_t *) -> clap_process_status { return CLAP_PROCESS_CONTINUE; },
      [](const clap_plugin_t *, const char *id) -> const void *
      { return std::strcmp(id, CLAP_EXT_PARAMS) == 0 ? &params : nullptr; },
      [](const clap_plugin_t *) {}};
    return &p->plugin;
  }};

static bool checkActivationNotifications()
{
  Clap::Library library;
  library._pluginFactory = &factory;
  library.plugins.push_back(&descriptor);
  HostContext context;
  Handler handler;
  auto storage = std::make_unique<ClapAsVst3>(&library, 0, nullptr);
  auto &wrapper = *storage;
  if (wrapper.initialize(&context) != kResultOk || wrapper.setComponentHandler(&handler) != kResultOk)
    return false;
  Vst::ProcessSetup setup{Vst::kRealtime, Vst::kSample32, 256, 48000};
  if (wrapper.setupProcessing(setup) != kResultOk) return false;
  const auto metadataFlags = Vst::kMidiCCAssignmentChanged | Vst::kParamValuesChanged |
                             Vst::kParamTitlesChanged;
  for (const auto state : {true, false})
  {
    handler.notifications.clear();
    handler.inActivation = true;
    if (wrapper.setActive(state) != kResultOk) return false;
    handler.inActivation = false;
    if (!handler.notifications.empty() || handler.reenteredActivation) return false;
    Vst::ParameterInfo info{};
    if (wrapper.getParameterInfo(0, info) != kResultOk ||
        info.title[0] != (state ? u'A' : u'D')) return false;
    wrapper.onIdle();
    const auto expected = state ? (metadataFlags & ~Vst::kMidiCCAssignmentChanged) | Vst::kLatencyChanged : metadataFlags;
    if (handler.notifications != std::vector<int32>{expected}) return false;
    wrapper.onIdle();
    if (handler.notifications.size() != 1 || handler.wrongThread) return false;
  }
  wrapper.setComponentHandler(nullptr);
  wrapper.terminate();
  return handler.references == 1;
}

static bool checkTailNotifications()
{
  Handler handler;
  auto storage = std::make_unique<ClapAsVst3>(nullptr, 0, nullptr);
  auto &wrapper = *storage;
  if (wrapper.setComponentHandler(&handler) != kResultOk) return false;
  const auto notifyFromAudioThread = [&]
  {
    std::thread audioThread([&]
    {
      wrapper.tail_changed();
      wrapper.tail_changed();
    });
    audioThread.join();
  };

  notifyFromAudioThread();
  if (!handler.notifications.empty() || handler.wrongThread) return false;
  wrapper.onIdle();
  if (handler.notifications != std::vector<int32>{Vst::kLatencyChanged} ||
      handler.wrongThread) return false;
  wrapper.onIdle();
  if (handler.notifications.size() != 1) return false;

  notifyFromAudioThread();
  if (wrapper.setComponentHandler(nullptr) != kResultOk) return false;
  wrapper.onIdle();
  if (handler.references != 1 || handler.notifications.size() != 1) return false;

  notifyFromAudioThread();
  wrapper.onIdle();
  if (wrapper.setComponentHandler(&handler) != kResultOk) return false;
  wrapper.onIdle();
  if (handler.notifications.size() != 1) return false;
  notifyFromAudioThread();
  wrapper.onIdle();
  if (handler.notifications != std::vector<int32>{Vst::kLatencyChanged, Vst::kLatencyChanged} ||
      handler.wrongThread) return false;
  wrapper.setComponentHandler(nullptr);
  return handler.references == 1;
}

static bool checkQueuedEditsAcrossRestart(bool replaceSchema)
{
  struct SynchronousHandler : Handler
  {
    ClapAsVst3 *wrapper = nullptr;
    bool deactivated = false;
    tresult PLUGIN_API restartComponent(int32 flags) override
    {
      Handler::restartComponent(flags);
      if (!deactivated && (flags & Vst::kLatencyChanged))
      {
        deactivated = true;
        if (wrapper->setActive(false) != kResultOk) return kResultFalse;
      }
      return kResultOk;
    }
  } handler;
  replaceParameterOnDeactivate = replaceSchema;
  Clap::Library library;
  library._pluginFactory = &factory;
  library.plugins.push_back(&descriptor);
  HostContext context;
  auto storage = std::make_unique<ClapAsVst3>(&library, 0, nullptr);
  auto &wrapper = *storage;
  handler.wrapper = &wrapper;
  if (wrapper.initialize(&context) != kResultOk || wrapper.setComponentHandler(&handler) != kResultOk)
    return false;
  Vst::ProcessSetup setup{Vst::kRealtime, Vst::kSample32, 256, 48000};
  if (wrapper.setupProcessing(setup) != kResultOk) return false;
  auto &automation = static_cast<Clap::IAutomation &>(wrapper);
  automation.onBeginEdit(7);
  wrapper.onIdle();
  if (handler.starts != std::vector<Vst::ParamID>{7}) return false;
  if (wrapper.setActive(true) != kResultOk) return false;
  clap_event_param_value_t event{};
  event.header.size = sizeof(event);
  event.header.type = CLAP_EVENT_PARAM_VALUE;
  event.param_id = 7;
  event.value = 0.4;
  automation.onPerformEdit(&event);
  automation.onEndEdit(7);
  wrapper.onIdle();
  if (!handler.deactivated
      || handler.edits != (replaceSchema ? std::vector<Vst::ParamID>{} : std::vector<Vst::ParamID>{7})
      || handler.ends != std::vector<Vst::ParamID>{7}) return false;
  event.param_id = replaceSchema ? 8 : 7;
  automation.onPerformEdit(&event);
  wrapper.onIdle();
  const auto expected = replaceSchema ? std::vector<Vst::ParamID>{8} : std::vector<Vst::ParamID>{7, 7};
  if (handler.edits != expected) return false;
  wrapper.setComponentHandler(nullptr);
  wrapper.terminate();
  replaceParameterOnDeactivate = false;
  return handler.references == 1;
}

int main()
{
  Handler handler;
  ClapAsVst3 wrapper(nullptr, 0, nullptr);

  wrapper.param_rescan(CLAP_PARAM_RESCAN_INFO);

  if (wrapper.setComponentHandler(&handler) != kResultOk)
    return 1;
  wrapper.param_rescan(CLAP_PARAM_RESCAN_INFO);
  if (handler.notifications != std::vector<int32>{
        Vst::RestartFlags::kParamValuesChanged | Vst::RestartFlags::kParamTitlesChanged})
    return 2;

  if (wrapper.setComponentHandler(nullptr) != kResultOk)
    return 3;
  wrapper.param_rescan(CLAP_PARAM_RESCAN_INFO);
  if (handler.references != 1 || handler.notifications.size() != 1)
    return 4;

  if (!checkActivationNotifications()) return 5;
  if (!checkTailNotifications()) return 6;
  if (!checkQueuedEditsAcrossRestart(false) || !checkQueuedEditsAcrossRestart(true)) return 7;

  std::puts("VST3 notifications and synchronous schema replacement pass; retired value edits are skipped, gesture endings preserved, current parameter edits delivered");
  return 0;
}
