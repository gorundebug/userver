#pragma once

#ifdef USERVER_TVM2_HTTP_CLIENT
#error Use clients::Http from clients/http.hpp instead
#endif

#include <clients/http/request.hpp>
#include <clients/http/statistics.hpp>

#include <moodycamel/concurrentqueue_fwd.h>
#include <utils/fast_pimpl.hpp>
#include <utils/periodic_task.hpp>
#include <utils/swappingsmart.hpp>
#include <utils/token_bucket.hpp>

namespace curl {
class easy;
class multi;
}  // namespace curl

namespace engine {
namespace ev {
class ThreadPool;
}  // namespace ev

class TaskProcessor;
}  // namespace engine

namespace clients::http {
namespace impl {
class EasyWrapper;
}  // namespace impl

class DestinationStatistics;
struct TestsuiteConfig;

class Client {
 public:
  /* Use this method to create Client */
  static std::shared_ptr<Client> Create(
      const std::string& thread_name_prefix, size_t io_threads,
      engine::TaskProcessor& fs_task_processor);

  Client(const Client&) = delete;
  Client(Client&&) = delete;
  ~Client();

  std::shared_ptr<Request> CreateRequest();

  /// Providing CreateNonSignedRequest() function for the clients::Http alias.
  std::shared_ptr<Request> CreateNotSignedRequest() { return CreateRequest(); }

  void SetMultiplexingEnabled(bool enabled);
  void SetMaxHostConnections(size_t max_host_connections);
  void SetConnectionPoolSize(size_t connection_pool_size);

  PoolStatistics GetPoolStatistics() const;

  /* Set max number of automatically created destination metrics */
  void SetDestinationMetricsAutoMaxSize(size_t max_size);

  const http::DestinationStatistics& GetDestinationStatistics() const;

  void SetTestsuiteConfig(const TestsuiteConfig& config);

  void SetConnectRatelimitHttp(
      size_t max_size, utils::TokenBucket::Duration token_update_interval);

  void SetConnectRatelimitHttps(
      size_t max_size, utils::TokenBucket::Duration token_update_interval);

 private:
  explicit Client(const std::string& thread_name_prefix, size_t io_threads,
                  engine::TaskProcessor& fs_task_processor);

  void ReinitEasy();

  InstanceStatistics GetMultiStatistics(size_t n) const;

  size_t FindMultiIndex(const curl::multi*) const;

  // Functions for EasyWrapper that must be noexcept, as they are called from
  // the EasyWrapper destructor.
  friend class impl::EasyWrapper;
  void IncPending() noexcept { ++pending_tasks_; }
  void DecPending() noexcept { --pending_tasks_; }
  void PushIdleEasy(std::shared_ptr<curl::easy>&& easy) noexcept;

  std::shared_ptr<curl::easy> TryDequeueIdle() noexcept;

  std::atomic<std::size_t> pending_tasks_{0};

  std::shared_ptr<DestinationStatistics> destination_statistics_;
  std::unique_ptr<engine::ev::ThreadPool> thread_pool_;
  std::vector<Statistics> statistics_;
  std::vector<std::unique_ptr<curl::multi>> multis_;

  static constexpr size_t kIdleQueueSize = 616;
  static constexpr size_t kIdleQueueAlignment = 8;
  using IdleQueueTraits = moodycamel::ConcurrentQueueDefaultTraits;
  using IdleQueueValue = std::shared_ptr<curl::easy>;
  using IdleQueue =
      moodycamel::ConcurrentQueue<IdleQueueValue, IdleQueueTraits>;
  utils::FastPimpl<IdleQueue, kIdleQueueSize, kIdleQueueAlignment> idle_queue_;

  engine::TaskProcessor& fs_task_processor_;
  utils::SwappingSmart<const curl::easy> easy_;
  utils::PeriodicTask easy_reinit_task_;

  // Testsuite support
  std::shared_ptr<const TestsuiteConfig> testsuite_config_;

  std::shared_ptr<utils::TokenBucket> http_connect_ratelimit_;
  std::shared_ptr<utils::TokenBucket> https_connect_ratelimit_;
};

}  // namespace clients::http
