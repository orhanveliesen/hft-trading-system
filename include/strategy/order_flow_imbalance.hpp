#pragma once

#include "../types.hpp"

#include <algorithm>

namespace hft {
namespace strategy {

/**
 * Order Flow Imbalance Strategy
 *
 * Mantık: Bid/Ask dengesizliği fiyat hareketini tahmin eder
 *
 * Imbalance = (BidQty - AskQty) / (BidQty + AskQty)
 *
 *   Imbalance > threshold  → Alıcılar güçlü → Fiyat çıkacak → AL
 *   Imbalance < -threshold → Satıcılar güçlü → Fiyat düşecek → SAT
 *
 * Çok kısa vadeli sinyal (microseconds to milliseconds)
 * HFT'nin en temel stratejilerinden biri
 */

enum class OFISignal : uint8_t { Hold = 0, Buy = 1, Sell = 2 };

struct OFIConfig {
    double imbalance_threshold = 0.3; // |0.3| = %30 dengesizlik
    Quantity min_total_qty = 100;     // Minimum toplam miktar (gürültü filtresi)
    Quantity order_size = 100;
    int64_t max_position = 1000;
};

class OrderFlowImbalance {
public:
    explicit OrderFlowImbalance(const OFIConfig& config = {}) : config_(config), last_imbalance_(0.0) {}

    // Ana sinyal fonksiyonu
    OFISignal operator()(Quantity bid_qty, Quantity ask_qty, int64_t position) {
        // Yeterli likidite var mı?
        Quantity total = bid_qty + ask_qty;
        if (total < config_.min_total_qty) {
            return OFISignal::Hold;
        }

        // Imbalance hesapla: [-1, +1] aralığında
        // +1 = sadece bid var (max alıcı baskısı)
        // -1 = sadece ask var (max satıcı baskısı)
        double imbalance = (double)((int64_t)bid_qty - (int64_t)ask_qty) / (double)total;
        last_imbalance_ = imbalance;

        // Sinyal üret
        if (imbalance > config_.imbalance_threshold) {
            // Alıcı baskısı yüksek → Fiyat çıkacak → AL
            if (position < config_.max_position) {
                return OFISignal::Buy;
            }
        } else if (imbalance < -config_.imbalance_threshold) {
            // Satıcı baskısı yüksek → Fiyat düşecek → SAT
            if (position > -config_.max_position) {
                return OFISignal::Sell;
            }
        }

        return OFISignal::Hold;
    }

    // Overload: bid/ask price'larla birlikte (spread kontrolü için)
    OFISignal operator()(Price bid, Price ask, Quantity bid_qty, Quantity ask_qty, int64_t position) {
        // Crossed market kontrolü
        if (bid >= ask || bid == INVALID_PRICE || ask == INVALID_PRICE) {
            return OFISignal::Hold;
        }
        return (*this)(bid_qty, ask_qty, position);
    }

    // Son hesaplanan imbalance (monitoring için)
    double last_imbalance() const { return last_imbalance_; }

    const OFIConfig& config() const { return config_; }

private:
    OFIConfig config_;
    double last_imbalance_;
};

/**
 * Multi-Level Order Flow Imbalance
 *
 * Sadece top-of-book değil, birden fazla seviyeyi analiz eder.
 * Daha güvenilir sinyal verir.
 */
struct MultiLevelOFIConfig {
    uint32_t num_levels = 5; // Kaç seviye analiz et
    double imbalance_threshold = 0.25;
    double level_weight_decay = 0.8; // Her seviyenin ağırlığı bir öncekinin 0.8x'i
    Quantity order_size = 100;
    int64_t max_position = 1000;
};

class MultiLevelOFI {
public:
    static constexpr size_t MAX_LEVELS = 10;

    explicit MultiLevelOFI(const MultiLevelOFIConfig& config = {}) : config_(config), last_imbalance_(0.0) {}

    // Çok seviyeli analiz
    OFISignal operator()(const Quantity* bid_qtys, const Quantity* ask_qtys, size_t num_levels, int64_t position) {
        if (num_levels == 0)
            return OFISignal::Hold;

        double weighted_bid = 0.0;
        double weighted_ask = 0.0;
        double weight = 1.0;
        double total_weight = 0.0;

        size_t levels = std::min(num_levels, (size_t)config_.num_levels);

        for (size_t i = 0; i < levels; ++i) {
            weighted_bid += bid_qtys[i] * weight;
            weighted_ask += ask_qtys[i] * weight;
            total_weight += weight;
            weight *= config_.level_weight_decay;
        }

        double total = weighted_bid + weighted_ask;
        if (total < 1.0)
            return OFISignal::Hold;

        double imbalance = (weighted_bid - weighted_ask) / total;
        last_imbalance_ = imbalance;

        if (imbalance > config_.imbalance_threshold) {
            if (position < config_.max_position) {
                return OFISignal::Buy;
            }
        } else if (imbalance < -config_.imbalance_threshold) {
            if (position > -config_.max_position) {
                return OFISignal::Sell;
            }
        }

        return OFISignal::Hold;
    }

    double last_imbalance() const { return last_imbalance_; }
    const MultiLevelOFIConfig& config() const { return config_; }

private:
    MultiLevelOFIConfig config_;
    double last_imbalance_;
};

} // namespace strategy
} // namespace hft
