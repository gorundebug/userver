#include <userver/taxi_config/storage/component.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <unordered_set>

#include <fmt/format.h>

#include <userver/components/component.hpp>
#include <userver/engine/condition_variable.hpp>
#include <userver/engine/mutex.hpp>
#include <userver/fs/blocking/read.hpp>
#include <userver/fs/read.hpp>
#include <userver/fs/write.hpp>
#include <userver/taxi_config/storage_mock.hpp>
#include <userver/tracing/span.hpp>
#include <userver/utils/assert.hpp>

#include <taxi_config/storage_data.hpp>

USERVER_NAMESPACE_BEGIN

namespace components {

namespace {

constexpr std::chrono::seconds kWaitInterval(5);

std::unordered_set<std::string>& AllConfigUpdaters() {
  // There may be components with the same name, but the component system would
  // not allow to use them at the same time. No need to register them twice.
  static std::unordered_set<std::string> config_updaters;
  return config_updaters;
}

}  // namespace

namespace impl {

void CheckRegistered(bool registered) {
  UINVARIANT(registered, "TaxiConfig update validation is broken");
}

}  // namespace impl

class TaxiConfig::Impl final {
 public:
  Impl(const ComponentConfig&, const ComponentContext&);

  taxi_config::Source GetSource();
  auto& GetChannel() { return cache_.channel; }

  void OnLoadingCancelled();

  void SetConfig(std::string_view updater, const taxi_config::DocsMap& value);
  void NotifyLoadingFailed(std::string_view updater, std::string_view error);

 private:
  void DoSetConfig(const taxi_config::DocsMap& value);

  bool Has() const;
  void WaitUntilLoaded();

  bool IsFsCacheEnabled() const;
  void ReadFsCache();
  void WriteFsCache(const taxi_config::DocsMap&);

 private:
  taxi_config::impl::StorageData cache_{taxi_config::impl::SnapshotData{{}}};

  const std::string fs_cache_path_;
  engine::TaskProcessor* fs_task_processor_;
  std::string fs_loading_error_msg_;

  std::atomic<bool> is_loaded_{false};
  mutable engine::Mutex loaded_mutex_;
  mutable engine::ConditionVariable loaded_cv_;
  bool config_load_cancelled_{false};
};

TaxiConfig::Impl::Impl(const ComponentConfig& config,
                       const ComponentContext& context)
    : cache_{taxi_config::impl::SnapshotData{{}}},
      fs_cache_path_(config["fs-cache-path"].As<std::string>()),
      fs_task_processor_(
          fs_cache_path_.empty()
              ? nullptr
              : &context.GetTaskProcessor(
                    config["fs-task-processor"].As<std::string>())) {
  std::vector<std::string> active_updaters;
  for (const auto& name : AllConfigUpdaters()) {
    if (context.Contains(name)) {
      active_updaters.push_back(name);
    }
  }

  UINVARIANT(active_updaters.size() < 2,
             fmt::format("Only one dynamic config updater should be "
                         "enabled, but multiple detected: {}",
                         fmt::join(active_updaters, ", ")));

  UINVARIANT(!active_updaters.empty(),
             fmt::format("There is no instance of a TaxiConfig::Updater. "
                         "At least one dynamic config updater should be "
                         "enabled to update the uninitialized TaxiConfig! "
                         "Add one of the updaters into the static "
                         "config: {}",
                         fmt::join(AllConfigUpdaters(), ", ")));

  ReadFsCache();
}

taxi_config::Source TaxiConfig::Impl::GetSource() {
  WaitUntilLoaded();
  return taxi_config::Source{cache_};
}

void TaxiConfig::Impl::WaitUntilLoaded() {
  if (Has()) return;

  LOG_TRACE() << "Wait started";
  {
    std::unique_lock lock(loaded_mutex_);
    while (!Has() && !config_load_cancelled_) {
      const auto res = loaded_cv_.WaitFor(lock, kWaitInterval);
      if (res == engine::CvStatus::kTimeout) {
        std::string_view fs_note = " Last error while reading config from FS: ";
        if (fs_loading_error_msg_.empty()) fs_note = {};
        LOG_WARNING() << "Waiting for the config load." << fs_note
                      << fs_loading_error_msg_;
      }
    }
  }
  LOG_TRACE() << "Wait finished";

  if (!Has() || config_load_cancelled_)
    throw ComponentsLoadCancelledException("config load cancelled");
}

void TaxiConfig::Impl::NotifyLoadingFailed(std::string_view updater,
                                           std::string_view error) {
  if (!Has()) {
    std::string message;
    if (!fs_loading_error_msg_.empty()) {
      message += fmt::format("TaxiConfig error: {}. ", fs_loading_error_msg_);
    }
    message += fmt::format("Error from '{}' updater: {}", updater, error);
    throw std::runtime_error(message);
  }
}

void TaxiConfig::Impl::DoSetConfig(const taxi_config::DocsMap& value) {
  auto config = taxi_config::impl::SnapshotData(value, {});
  {
    std::lock_guard lock(loaded_mutex_);
    cache_.config.Assign(std::move(config));
    is_loaded_ = true;
  }
  loaded_cv_.NotifyAll();
  cache_.channel.SendEvent(taxi_config::Source{cache_}.GetSnapshot());
}

void TaxiConfig::Impl::SetConfig(std::string_view updater,
                                 const taxi_config::DocsMap& value) {
  LOG_DEBUG() << "Setting new dynamic config value from '" << updater << "'";
  DoSetConfig(value);
  WriteFsCache(value);
}

void TaxiConfig::Impl::OnLoadingCancelled() {
  if (Has()) return;

  LOG_ERROR() << "Config was set to nullptr, config load cancelled";
  {
    std::lock_guard<engine::Mutex> lock(loaded_mutex_);
    config_load_cancelled_ = true;
  }
  loaded_cv_.NotifyAll();
}

bool TaxiConfig::Impl::Has() const { return is_loaded_.load(); }

bool TaxiConfig::Impl::IsFsCacheEnabled() const {
  UASSERT_MSG(fs_cache_path_.empty() || fs_task_processor_,
              "fs_task_processor_ must be set if there is a fs_cache_path_");
  return !fs_cache_path_.empty();
}

void TaxiConfig::Impl::ReadFsCache() {
  if (!IsFsCacheEnabled()) return;

  tracing::Span span("taxi_config_fs_cache_read");
  try {
    if (!fs::FileExists(*fs_task_processor_, fs_cache_path_)) {
      LOG_WARNING() << "No FS cache for config found, waiting until "
                       "TaxiConfigClientUpdater fetches fresh configs";
      return;
    }
    const auto contents =
        fs::ReadFileContents(*fs_task_processor_, fs_cache_path_);

    taxi_config::DocsMap docs_map;
    docs_map.Parse(contents, false);

    DoSetConfig(docs_map);
    LOG_INFO() << "Successfully read taxi_config from FS cache";
  } catch (const std::exception& e) {
    /* Possible sources of error:
     * 1) cache file suddenly disappeared
     * 2) cache file is broken
     * 3) cache file is created by an old server version, it misses some
     * variables which are needed in current server version
     */
    fs_loading_error_msg_ = fmt::format(
        "Failed to load config from FS cache '{}'. ", fs_cache_path_);
    LOG_WARNING() << fs_loading_error_msg_ << e;
    fs_loading_error_msg_ += e.what();
  }
}

void TaxiConfig::Impl::WriteFsCache(const taxi_config::DocsMap& docs_map) {
  if (!IsFsCacheEnabled()) return;

  tracing::Span span("taxi_config_fs_cache_write");
  try {
    const auto contents = docs_map.AsJsonString();
    using perms = boost::filesystem::perms;
    auto mode = perms::owner_read | perms::owner_write | perms::group_read |
                perms::others_read;
    fs::CreateDirectories(
        *fs_task_processor_,
        boost::filesystem::path(fs_cache_path_).parent_path().string());
    fs::RewriteFileContentsAtomically(*fs_task_processor_, fs_cache_path_,
                                      contents, mode);

    LOG_INFO() << "Successfully wrote taxi_config from FS cache";
  } catch (const std::exception& e) {
    LOG_ERROR() << "Failed to save config to FS cache '" << fs_cache_path_
                << "': " << e;
  }
}

TaxiConfigFallbacksComponent::TaxiConfigFallbacksComponent(
    const ComponentConfig& config, const ComponentContext& context)
    : LoggableComponentBase(config, context), updater_(context) {
  try {
    auto fallback_config_contents = fs::blocking::ReadFileContents(
        config["fallback-path"].As<std::string>());

    taxi_config::DocsMap fallback_config;
    fallback_config.Parse(fallback_config_contents, false);

    updater_.SetConfig(fallback_config);
  } catch (const std::exception& ex) {
    LOG_ERROR() << "Cannot load fallback config: " << ex;
    updater_.NotifyLoadingFailed(ex.what());
  }
}

TaxiConfig::NoblockSubscriber::NoblockSubscriber(
    TaxiConfig& config_component) noexcept
    : config_component_(config_component) {}

concurrent::AsyncEventSource<const taxi_config::Snapshot&>&
TaxiConfig::NoblockSubscriber::GetEventSource() noexcept {
  return config_component_.impl_->GetChannel();
}

TaxiConfig::TaxiConfig(const ComponentConfig& config,
                       const ComponentContext& context)
    : LoggableComponentBase(config, context), impl_(config, context) {}

TaxiConfig::~TaxiConfig() = default;

taxi_config::Source TaxiConfig::GetSource() { return impl_->GetSource(); }

bool TaxiConfig::RegisterUpdaterName(std::string_view name) {
  auto& names = AllConfigUpdaters();
  names.emplace(name);
  return !names.empty();
}

TaxiConfig& TaxiConfig::GetTaxiConfig(const ComponentContext& context) {
  return context.FindComponent<components::TaxiConfig>();
}

void TaxiConfig::OnLoadingCancelled() { impl_->OnLoadingCancelled(); }

void TaxiConfig::SetConfig(std::string_view updater,
                           const taxi_config::DocsMap& value) {
  impl_->SetConfig(updater, value);
}

void TaxiConfig::NotifyLoadingFailed(std::string_view updater,
                                     std::string_view error) {
  impl_->NotifyLoadingFailed(updater, error);
}

}  // namespace components

USERVER_NAMESPACE_END
