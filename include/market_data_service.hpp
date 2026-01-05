#pragma once

#include "types.hpp"
#include "orderbook.hpp"
#include "matching_engine.hpp"
#include "feed_handler.hpp"
#include "network/udp_receiver.hpp"
#include "network/packet_buffer.hpp"
#include <unordered_map>
#include <thread>
#include <atomic>
#include <functional>

namespace hft {

// Market data update callback
struct MarketUpdate {
    Symbol symbol;
    Price best_bid;
    Price best_ask;
    Quantity bid_size;
    Quantity ask_size;
    Timestamp timestamp;
};

using MarketUpdateCallback = std::function<void(const MarketUpdate&)>;

// Integrated market data service
// - Receives UDP multicast packets
// - Parses ITCH messages
// - Updates order books
// - Notifies strategy on BBO changes
class MarketDataService {
public:
    static constexpr size_t MAX_SYMBOLS = 10000;
    static constexpr size_t PACKET_BUFFER_SIZE = 65536;

    MarketDataService()
        : running_(false)
        , packets_received_(0)
        , messages_processed_(0)
    {}

    ~MarketDataService() {
        stop();
    }

    // Add symbol to track
    void add_symbol(Symbol symbol_id, const std::string& ticker,
                    Price base_price = 90000, size_t price_range = 200000) {
        auto book = std::make_unique<OrderBook>(base_price, price_range);
        order_books_[symbol_id] = std::move(book);
        ticker_to_symbol_[ticker] = symbol_id;
    }

    // Set callback for market updates
    void set_update_callback(MarketUpdateCallback callback) {
        update_callback_ = std::move(callback);
    }

    // Initialize UDP receiver
    bool init(const network::UdpConfig& config) {
        return receiver_.init(config);
    }

    // Start processing (non-blocking, starts threads)
    void start() {
        if (running_) return;
        running_ = true;

        // Start receiver thread
        receiver_thread_ = std::thread([this]() {
            run_receiver();
        });

        // Start processor thread
        processor_thread_ = std::thread([this]() {
            run_processor();
        });
    }

    // Stop processing
    void stop() {
        running_ = false;
        receiver_.stop();

        if (receiver_thread_.joinable()) {
            receiver_thread_.join();
        }
        if (processor_thread_.joinable()) {
            processor_thread_.join();
        }
    }

    // Get order book for symbol
    OrderBook* get_order_book(Symbol symbol) {
        auto it = order_books_.find(symbol);
        return it != order_books_.end() ? it->second.get() : nullptr;
    }

    // Statistics
    uint64_t packets_received() const { return packets_received_; }
    uint64_t messages_processed() const { return messages_processed_; }

    // Generic callback interface (called by FeedHandler)
    // Note: For MarketDataService, we need symbol tracking which requires
    // the raw ITCH data. This is a special case - most callbacks don't need this.
    void on_add_order(OrderId order_id, Side side, Price price, Quantity qty) {
        // For multi-symbol support, we'd need symbol info passed here
        // For now, using first registered symbol (single-symbol mode)
        if (order_books_.empty()) return;

        auto& book = order_books_.begin()->second;
        Symbol symbol = order_books_.begin()->first;

        book->add_order(order_id, side, price, qty);
        order_to_symbol_[order_id] = symbol;

        notify_update(symbol, book.get());
        ++messages_processed_;
    }

    void on_order_executed(OrderId order_id, Quantity qty) {
        Symbol symbol = lookup_order_symbol(order_id);
        if (symbol == 0) return;

        auto* book = get_order_book(symbol);
        if (!book) return;

        book->execute_order(order_id, qty);
        notify_update(symbol, book);
        ++messages_processed_;
    }

    void on_order_cancelled(OrderId order_id, Quantity qty) {
        Symbol symbol = lookup_order_symbol(order_id);
        if (symbol == 0) return;

        auto* book = get_order_book(symbol);
        if (!book) return;

        book->execute_order(order_id, qty);
        notify_update(symbol, book);
        ++messages_processed_;
    }

    void on_order_deleted(OrderId order_id) {
        Symbol symbol = lookup_order_symbol(order_id);
        if (symbol == 0) return;

        auto* book = get_order_book(symbol);
        if (!book) return;

        book->cancel_order(order_id);
        order_to_symbol_.erase(order_id);
        notify_update(symbol, book);
        ++messages_processed_;
    }

private:
    // Core components
    network::UdpReceiver receiver_;
    network::PacketBuffer<1500, PACKET_BUFFER_SIZE> packet_buffer_;

    // Order books per symbol
    std::unordered_map<Symbol, std::unique_ptr<OrderBook>> order_books_;

    // Symbol lookup tables
    std::unordered_map<std::string, Symbol> ticker_to_symbol_;
    std::unordered_map<OrderId, Symbol> order_to_symbol_;

    // Callback
    MarketUpdateCallback update_callback_;

    // Threading
    std::atomic<bool> running_;
    std::thread receiver_thread_;
    std::thread processor_thread_;

    // Statistics
    std::atomic<uint64_t> packets_received_;
    std::atomic<uint64_t> messages_processed_;

    // Receiver thread - pulls packets from network
    void run_receiver() {
        while (running_) {
            receiver_.poll([this](const uint8_t* data, size_t len) {
                packet_buffer_.push(data, len);
                ++packets_received_;
            }, 1000);  // 1ms timeout
        }
    }

    // Processor thread - processes packets from buffer
    void run_processor() {
        FeedHandler<MarketDataService> handler(*this);

        while (running_) {
            while (auto* pkt = packet_buffer_.front()) {
                // Skip MoldUDP header (20 bytes) and process messages
                if (pkt->len > 20) {
                    auto header = network::parse_moldudp_header(pkt->data);
                    const uint8_t* msg_data = pkt->data + 20;
                    size_t remaining = pkt->len - 20;

                    // Process each message in packet
                    for (uint16_t i = 0; i < header.message_count && remaining > 2; ++i) {
                        uint16_t msg_len = network::parse_moldudp_header(msg_data).message_count;
                        // Actually message length is in first 2 bytes
                        msg_len = (static_cast<uint16_t>(msg_data[0]) << 8) | msg_data[1];

                        if (msg_len > remaining - 2) break;

                        handler.process_message(msg_data + 2, msg_len);

                        msg_data += 2 + msg_len;
                        remaining -= 2 + msg_len;
                    }
                }
                packet_buffer_.pop();
            }

            // Brief sleep if no packets
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }

    Symbol lookup_symbol(const char* ticker) {
        std::string t(ticker, 8);
        // Trim trailing spaces
        t.erase(t.find_last_not_of(' ') + 1);

        auto it = ticker_to_symbol_.find(t);
        return it != ticker_to_symbol_.end() ? it->second : 0;
    }

    Symbol lookup_order_symbol(OrderId order_ref) {
        auto it = order_to_symbol_.find(order_ref);
        return it != order_to_symbol_.end() ? it->second : 0;
    }

    void notify_update(Symbol symbol, OrderBook* book) {
        if (!update_callback_) return;

        MarketUpdate update;
        update.symbol = symbol;
        update.best_bid = book->best_bid();
        update.best_ask = book->best_ask();
        update.bid_size = book->bid_quantity_at(update.best_bid);
        update.ask_size = book->ask_quantity_at(update.best_ask);
        update.timestamp = 0;  // TODO: Add timestamp

        update_callback_(update);
    }
};

}  // namespace hft
