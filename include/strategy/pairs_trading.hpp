#pragma once

#include "../types.hpp"
#include <cmath>

namespace hft {
namespace strategy {

/**
 * Pairs Trading / Statistical Arbitrage
 *
 * Mantık: İki ilişkili varlık arasındaki spread'i trade et
 *
 * Örnek: AAPL vs MSFT
 *   - Normalde spread = AAPL - 1.2 × MSFT (hedge ratio)
 *   - Spread ortalamanın 2σ üstüne çıktı → Short AAPL, Long MSFT
 *   - Spread ortalamanın 2σ altına düştü → Long AAPL, Short MSFT
 *
 * Mean reversion'ın çift varlıklı versiyonu
 */

struct PairSignal {
    bool should_trade = false;
    bool long_first = false;   // true: Long A, Short B | false: Short A, Long B
    Quantity quantity = 0;
};

struct PairsConfig {
    double hedge_ratio = 1.0;        // B'nin kaç katı A'yı hedge eder
    double entry_zscore = 2.0;       // Pozisyon aç (2 sigma)
    double exit_zscore = 0.5;        // Pozisyon kapat (0.5 sigma)
    uint32_t lookback = 100;         // Mean/std hesaplama penceresi
    Quantity order_size = 100;
    int64_t max_position = 1000;
};

class PairsTrading {
public:
    static constexpr size_t MAX_LOOKBACK = 256;

    explicit PairsTrading(const PairsConfig& config = {})
        : config_(config)
        , spreads_{}
        , head_(0)
        , count_(0)
        , in_position_(false)
        , position_is_long_first_(false)
    {
        if (config_.lookback > MAX_LOOKBACK) {
            config_.lookback = MAX_LOOKBACK;
        }
    }

    // Spread güncelle ve sinyal üret
    PairSignal operator()(Price price_a, Price price_b, int64_t current_position) {
        PairSignal signal;

        if (price_a == INVALID_PRICE || price_b == INVALID_PRICE) {
            return signal;
        }

        // Spread hesapla: A - hedge_ratio × B
        double spread = (double)price_a - config_.hedge_ratio * (double)price_b;

        // Circular buffer'a ekle
        spreads_[head_] = spread;
        head_ = (head_ + 1) % config_.lookback;
        if (count_ < config_.lookback) {
            ++count_;
            return signal;  // Yeterli veri yok
        }

        // Mean ve std hesapla
        double mean = calculate_mean();
        double std = calculate_std(mean);

        if (std < 0.0001) return signal;  // Volatilite çok düşük

        // Z-score hesapla
        double zscore = (spread - mean) / std;

        // Pozisyon yönetimi
        if (!in_position_) {
            // Pozisyon yok, giriş sinyali ara
            if (zscore > config_.entry_zscore) {
                // Spread çok yüksek → Short A, Long B
                if (current_position > -config_.max_position) {
                    signal.should_trade = true;
                    signal.long_first = false;  // Short A, Long B
                    signal.quantity = config_.order_size;
                    in_position_ = true;
                    position_is_long_first_ = false;
                }
            } else if (zscore < -config_.entry_zscore) {
                // Spread çok düşük → Long A, Short B
                if (current_position < config_.max_position) {
                    signal.should_trade = true;
                    signal.long_first = true;  // Long A, Short B
                    signal.quantity = config_.order_size;
                    in_position_ = true;
                    position_is_long_first_ = true;
                }
            }
        } else {
            // Pozisyon var, çıkış sinyali ara
            bool should_exit = false;

            if (position_is_long_first_) {
                // Long A, Short B pozisyonundayız
                // Spread normale döndü veya ters yöne gitti
                should_exit = (zscore > -config_.exit_zscore);
            } else {
                // Short A, Long B pozisyonundayız
                should_exit = (zscore < config_.exit_zscore);
            }

            if (should_exit) {
                signal.should_trade = true;
                signal.long_first = !position_is_long_first_;  // Ters pozisyon
                signal.quantity = config_.order_size;
                in_position_ = false;
            }
        }

        return signal;
    }

    // Durum sorgulama
    bool in_position() const { return in_position_; }
    double current_zscore() const {
        if (count_ < config_.lookback) return 0.0;
        double mean = calculate_mean();
        double std = calculate_std(mean);
        if (std < 0.0001) return 0.0;
        return (spreads_[(head_ + config_.lookback - 1) % config_.lookback] - mean) / std;
    }

    const PairsConfig& config() const { return config_; }

    void reset() {
        head_ = 0;
        count_ = 0;
        in_position_ = false;
    }

private:
    double calculate_mean() const {
        double sum = 0.0;
        for (size_t i = 0; i < config_.lookback; ++i) {
            sum += spreads_[i];
        }
        return sum / config_.lookback;
    }

    double calculate_std(double mean) const {
        double sum_sq = 0.0;
        for (size_t i = 0; i < config_.lookback; ++i) {
            double diff = spreads_[i] - mean;
            sum_sq += diff * diff;
        }
        return std::sqrt(sum_sq / config_.lookback);
    }

    PairsConfig config_;
    double spreads_[MAX_LOOKBACK];
    size_t head_;
    size_t count_;
    bool in_position_;
    bool position_is_long_first_;
};

}  // namespace strategy
}  // namespace hft
