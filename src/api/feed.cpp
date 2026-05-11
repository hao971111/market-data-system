#include "mds/feed.h"

#include <atomic>
#include <iostream>
#include <memory>
#include <utility>

#include "common/types.h"
#include "config/config.h"
#include "monitor/metrics.h"
#include "network/connection_manager.h"
#include "storage/binary_order_book_writer.h"
#include "storage/binary_trade_writer.h"

namespace mds {

namespace {

// writer 队列容量。Feed 第一版不暴露给用户（与 main.cpp 默认一致）。
constexpr std::size_t kWriterQueueCapacity = 100000;

}  // namespace

class MarketDataFeed::Impl {
public:
    explicit Impl(FeedConfig cfg)
        : cfg_(std::move(cfg)),
          // persist_ 是 const，构造后永不变，避免和 metrics_snapshot() 并发读时的 race。
          persist_(!cfg_.data_dir.empty())
    {
        // 把外部 FeedConfig 投影到内部 Config。只搬 ConnectionManager 实际会读到
        // 的字段（参考 src/network/connection_manager.cpp 里 config_.* 的访问），
        // 其他字段（pin_cpu、max_reconnect_attempts 等）保留默认值。
        internal_cfg_.symbols              = cfg_.symbols;
        internal_cfg_.proxy_url            = cfg_.proxy_url;
        internal_cfg_.data_dir             = cfg_.data_dir;
        internal_cfg_.reconnect_interval_ms = cfg_.reconnect_interval_ms;
        internal_cfg_.ping_interval_ms      = cfg_.ping_interval_ms;
        internal_cfg_.no_data_timeout_ms    = cfg_.no_data_timeout_ms;
        // 保持 internal Config 自洽：ws_url 既用于 cm_->start(cfg_.ws_url)，
        // 也同步到 data_sources[0]，避免后续若内部改为读 data_sources 时出现
        // “外部 ws_url 生效但内部快照不一致”的排障困扰。
        internal_cfg_.data_sources.clear();
        internal_cfg_.data_sources.push_back(
            DataSourceConfig{"mds_feed_primary", cfg_.ws_url, 0, true});

        // ConnectionManager 持有 Config& / Metrics& 的引用；Impl 在的话它们就在。
        cm_ = std::make_unique<ConnectionManager>(internal_cfg_, metrics_);

        // writer 一律在构造时分配，让 unique_ptr 全生命周期保持非空——
        // metrics_snapshot() 跨线程读它时不再有 unique_ptr 本身的 race。
        // 是否真落盘由 const persist_ 决定；start() 只负责调 open()。
        trade_writer_     = std::make_unique<BinaryTradeWriter>();
        orderbook_writer_ = std::make_unique<BinaryOrderBookWriter>();
    }

    ~Impl() {
        // 析构里调用 stop() 必须吞异常：C++ 析构默认 noexcept(true)，
        // 一旦抛异常 std::terminate 会直接打死进程。诊断信息走 cerr。
        try {
            stop();
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] MarketDataFeed::~Impl: stop() threw: "
                      << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[ERROR] MarketDataFeed::~Impl: stop() threw unknown"
                      << std::endl;
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void set_on_trade(TradeCallback cb)         { on_trade_      = std::move(cb); }
    void set_on_orderbook(OrderBookCallback cb) { on_orderbook_  = std::move(cb); }

    void start() {
        // 重复 start 安全：第二次直接 no-op。
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true)) {
            return;
        }

        // 启动序列里任何一步抛异常，都应该回滚到"未启动"状态：
        //   - started_ 必须恢复 false（否则 stop() 看作没启动而短路，资源永远不清）
        //   - 已部分起来的 writer / cm 也要 close / join
        // stop() 已经把这三件事打包做完且幂等，直接复用。
        try {
            // persist_=true 时打开 writer；open 失败仅 cerr，不 reset 指针——
            // 保持 unique_ptr 稳定。未 open 的 writer 内部 running_=false，
            // write() / records_written() / records_dropped() 全部安全降级。
            if (persist_) {
                if (!trade_writer_->open(cfg_.data_dir, kWriterQueueCapacity)) {
                    std::cerr << "[ERROR] MarketDataFeed: failed to open trade writer in "
                              << cfg_.data_dir << std::endl;
                }
                if (!orderbook_writer_->open(cfg_.data_dir, kWriterQueueCapacity)) {
                    std::cerr << "[ERROR] MarketDataFeed: failed to open orderbook writer in "
                              << cfg_.data_dir << std::endl;
                }
            }

            // 把内部回调注册给 ConnectionManager：先落盘（如启用），再触发用户回调。
            // 用户回调用 try/catch 包住，避免一个用户 bug 把整个 IO 线程打死。
            cm_->set_trade_callback([this](const Trade& t) {
            if (persist_) {
                trade_writer_->write(t);  // 失败/丢弃由 writer 内部计入 dropped
            }
            if (on_trade_) {
                try {
                    on_trade_(t);
                } catch (const std::exception& e) {
                    metrics_.callback_errors.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "[ERROR] MarketDataFeed on_trade callback: "
                              << e.what() << std::endl;
                } catch (...) {
                    metrics_.callback_errors.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "[ERROR] MarketDataFeed on_trade callback: unknown"
                              << std::endl;
                }
            }
        });

            cm_->set_orderbook_callback([this](const OrderBookSnapshot& ob) {
                if (persist_) {
                    orderbook_writer_->write(ob);
                }
                if (on_orderbook_) {
                    try {
                        on_orderbook_(ob);
                    } catch (const std::exception& e) {
                        metrics_.callback_errors.fetch_add(1, std::memory_order_relaxed);
                        std::cerr << "[ERROR] MarketDataFeed on_orderbook callback: "
                                  << e.what() << std::endl;
                    } catch (...) {
                        metrics_.callback_errors.fetch_add(1, std::memory_order_relaxed);
                        std::cerr << "[ERROR] MarketDataFeed on_orderbook callback: unknown"
                                  << std::endl;
                    }
                }
            });

            cm_->start(cfg_.ws_url);
        } catch (...) {
            // start() 抛异常 → 回滚。stop() 内部也可能抛，再吞一层
            // （析构链路上 stop 抛出已经在 ~Impl 里被吞掉，这里同理保证 noexcept-like）。
            try { stop(); } catch (...) {}
            throw;
        }
    }

    void stop() {
        // 重复 stop 安全。析构时也会自动调用。
        bool expected = true;
        if (!started_.compare_exchange_strong(expected, false)) {
            return;
        }
        cm_->stop();
        // close() 是幂等的，未 open 状态下也安全调用。
        trade_writer_->close();
        orderbook_writer_->close();
    }

    MetricsSnapshot metrics_snapshot() const {
        MetricsSnapshot s;
        s.messages_received    = metrics_.msgs_recv.load(std::memory_order_relaxed);
        s.trades_received      = metrics_.trades_parsed.load(std::memory_order_relaxed);
        s.orderbooks_received  = metrics_.orderbooks_parsed.load(std::memory_order_relaxed);
        // unique_ptr 自构造起一直非空，写盘禁用时 writer 内部计数恒为 0。
        s.trades_written       = trade_writer_->records_written();
        s.orderbooks_written   = orderbook_writer_->records_written();
        s.trades_dropped       = trade_writer_->records_dropped();
        s.orderbooks_dropped   = orderbook_writer_->records_dropped();
        s.connect_attempts     = metrics_.connect_attempts.load(std::memory_order_relaxed);
        s.connect_success      = metrics_.connect_successes.load(std::memory_order_relaxed);
        s.pipeline_over_500us  = metrics_.pipeline_over_500us.load(std::memory_order_relaxed);
        s.pipeline_over_1000us = metrics_.pipeline_over_1000us.load(std::memory_order_relaxed);
        s.clock_anomaly_count  = metrics_.clock_anomaly_count.load(std::memory_order_relaxed);
        return s;
    }

    const FeedConfig& config() const noexcept { return cfg_; }

private:
    FeedConfig cfg_;
    const bool persist_;  // = !cfg_.data_dir.empty()，构造后永不变
    Config     internal_cfg_;
    Metrics    metrics_;

    std::unique_ptr<ConnectionManager>     cm_;
    std::unique_ptr<BinaryTradeWriter>     trade_writer_;
    std::unique_ptr<BinaryOrderBookWriter> orderbook_writer_;

    TradeCallback      on_trade_;
    OrderBookCallback  on_orderbook_;
    std::atomic<bool> started_{false};
};

// pImpl 必须把析构 / move 放在 .cpp，原因同 Replayer。
MarketDataFeed::MarketDataFeed(FeedConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

MarketDataFeed::~MarketDataFeed() = default;
MarketDataFeed::MarketDataFeed(MarketDataFeed&&) noexcept = default;
MarketDataFeed& MarketDataFeed::operator=(MarketDataFeed&&) noexcept = default;

void MarketDataFeed::set_on_trade(TradeCallback cb) {
    impl_->set_on_trade(std::move(cb));
}
void MarketDataFeed::set_on_orderbook(OrderBookCallback cb) {
    impl_->set_on_orderbook(std::move(cb));
}

void MarketDataFeed::start() { impl_->start(); }
void MarketDataFeed::stop()  { impl_->stop(); }

MetricsSnapshot MarketDataFeed::metrics_snapshot() const {
    return impl_->metrics_snapshot();
}

const FeedConfig& MarketDataFeed::config() const noexcept {
    return impl_->config();
}

}  // namespace mds
