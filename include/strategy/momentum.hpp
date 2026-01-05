#pragma once

#include "../types.hpp"

namespace hft {
namespace strategy {

/**
 * Momentum Strategy
 *
 * Mantık: Trend takibi
 *   - Fiyat N tick'te X bps çıktı → Al (trend devam edecek)
 *   - Fiyat N tick'te X bps düştü → Sat (trend devam edecek)
 *
 * Mean Reversion'ın tam tersi!
 * Volatil piyasalarda iyi çalışır.
 */

enum class MomentumSignal : uint8_t {
    Hold = 0,
    Buy  = 1,
    Sell = 2
};

struct MomentumConfig {
    uint32_t lookback_ticks = 10;     // Kaç tick geriye bak
    uint32_t threshold_bps = 10;      // Sinyal eşiği (basis points)
    Quantity order_size = 100;
    int64_t max_position = 1000;
};

class MomentumStrategy {
public:
    static constexpr size_t MAX_LOOKBACK = 64;

    explicit MomentumStrategy(const MomentumConfig& config = {})
        : config_(config)
        , prices_{}
        , head_(0)
        , count_(0)
    {
        // Lookback limitini kontrol et
        if (config_.lookback_ticks > MAX_LOOKBACK) {
            config_.lookback_ticks = MAX_LOOKBACK;
        }
    }

    MomentumSignal operator()(Price bid, Price ask, int64_t position) {
        if (bid == INVALID_PRICE || ask == INVALID_PRICE || bid >= ask) {
            return MomentumSignal::Hold;
        }

        Price mid = (bid + ask) / 2;

        // Circular buffer'a ekle
        prices_[head_] = mid;
        head_ = (head_ + 1) % config_.lookback_ticks;
        if (count_ < config_.lookback_ticks) {
            ++count_;
            return MomentumSignal::Hold;  // Yeterli veri yok
        }

        // En eski fiyatı al (head şu an en eski noktaya işaret ediyor)
        Price oldest = prices_[head_];

        // Momentum hesapla (basis points)
        // Pozitif = yukarı trend, Negatif = aşağı trend
        int64_t momentum_bps = ((int64_t)mid - (int64_t)oldest) * 10000 / (int64_t)oldest;

        // Sinyal üret
        if (momentum_bps >= (int64_t)config_.threshold_bps) {
            // Güçlü yukarı momentum → AL
            if (position < config_.max_position) {
                return MomentumSignal::Buy;
            }
        } else if (momentum_bps <= -(int64_t)config_.threshold_bps) {
            // Güçlü aşağı momentum → SAT
            if (position > -config_.max_position) {
                return MomentumSignal::Sell;
            }
        }

        return MomentumSignal::Hold;
    }

    // Mevcut momentum değerini al (debug/monitoring için)
    int64_t current_momentum_bps() const {
        if (count_ < config_.lookback_ticks) return 0;

        Price newest = prices_[(head_ + config_.lookback_ticks - 1) % config_.lookback_ticks];
        Price oldest = prices_[head_];

        if (oldest == 0) return 0;
        return ((int64_t)newest - (int64_t)oldest) * 10000 / (int64_t)oldest;
    }

    const MomentumConfig& config() const { return config_; }
    void reset() { head_ = 0; count_ = 0; }

private:
    MomentumConfig config_;
    Price prices_[MAX_LOOKBACK];
    size_t head_;
    size_t count_;
};

}  // namespace strategy
}  // namespace hft
