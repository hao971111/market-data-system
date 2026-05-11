// examples/replay_only.cpp
//
// 演示：用 mds 库做一次最小回放。
// 编译产物：build/replay_only
//
// 用法：
//   ./build/replay_only [data_dir]
//   缺省 data_dir 为 "./data"。
//
// 运行前需要先用主程序至少跑过一次 live 模式产出 trades.bin / orderbooks.bin，
// 例如：./build/market-data-system 然后 Ctrl+C。

#include <cstdint>
#include <iostream>
#include <string>

#include <mds/replayer.h>
#include <mds/types.h>

namespace {

void print_result(const char* tag, const mds::Replayer::Result& r) {
    std::cout << "[example] " << tag << " summary:"
              << " header=" << r.header_record_count
              << " replayed=" << r.records_replayed
              << " completed=" << (r.completed ? "yes" : "no")
              << " file_error=" << (r.file_error ? "yes" : "no")
              << " count_mismatch=" << (r.count_mismatch ? "yes" : "no")
              << " callback_error=" << (r.callback_error ? "yes" : "no")
              << std::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string data_dir = (argc > 1) ? argv[1] : "./data";

    mds::Replayer replayer(data_dir);
    std::cout << "[example] data_dir=" << replayer.data_dir() << std::endl;

    uint64_t trade_seen = 0;
    auto tr = replayer.replay_trades([&](const mds::Trade& t) {
        ++trade_seen;
        if (trade_seen <= 3) {
            std::cout << "[trade] ts=" << t.timestamp_us
                      << " " << t.symbol
                      << " price=" << t.price
                      << " qty=" << t.quantity << std::endl;
        }
    });
    print_result("trade", tr);

    uint64_t book_seen = 0;
    auto br = replayer.replay_orderbooks([&](const mds::OrderBookSnapshot& ob) {
        ++book_seen;
        if (book_seen <= 3) {
            std::cout << "[book ] ts=" << ob.timestamp_us
                      << " " << ob.symbol
                      << " bid=" << ob.best_bid_price()
                      << " ask=" << ob.best_ask_price() << std::endl;
        }
    });
    print_result("orderbook", br);

    return (tr.completed && br.completed) ? 0 : 1;
}
