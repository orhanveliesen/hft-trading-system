#pragma once

#include "../types.hpp"

namespace hft {
namespace strategy {

/**
 * VWAP Execution Strategy
 *
 * Amaç: Büyük bir emri VWAP'a yakın fiyattan execute etmek
 *
 * VWAP = Σ(Price × Volume) / Σ(Volume)
 *
 * Mantık:
 *   - Mevcut fiyat < VWAP → Al (ucuz)
 *   - Mevcut fiyat > VWAP → Sat (pahalı)
 *
 * Genelde institutional trading'de kullanılır.
 * "Ben 100,000 lot almak istiyorum, VWAP'ın altında al"
 */

struct VWAPConfig {
    Quantity target_quantity = 10000; // Toplam almak/satmak istenen miktar
    Quantity slice_size = 100;        // Her seferde ne kadar
    uint32_t threshold_bps = 5;       // VWAP'tan sapma eşiği
    bool is_buy = true;               // Alım mı satım mı
};

struct VWAPSignal {
    bool should_trade = false;
    Quantity quantity = 0;
    Price limit_price = 0;
};

class VWAPStrategy {
public:
    explicit VWAPStrategy(const VWAPConfig& config = {})
        : config_(config), cumulative_pv_(0) // Price × Volume toplamı
          ,
          cumulative_volume_(0), executed_quantity_(0) {}

    // Market data güncelle (her trade'de çağrılır)
    void on_trade(Price price, Quantity volume) {
        cumulative_pv_ += (uint64_t)price * volume;
        cumulative_volume_ += volume;
    }

    // VWAP hesapla
    Price vwap() const {
        if (cumulative_volume_ == 0)
            return INVALID_PRICE;
        return static_cast<Price>(cumulative_pv_ / cumulative_volume_);
    }

    // Sinyal üret
    VWAPSignal operator()(Price bid, Price ask) {
        VWAPSignal signal;

        // Hedef tamamlandı mı?
        if (executed_quantity_ >= config_.target_quantity) {
            return signal; // İş bitti
        }

        // VWAP hesapla
        Price current_vwap = vwap();
        if (current_vwap == INVALID_PRICE) {
            return signal; // Yeterli veri yok
        }

        Price mid = (bid + ask) / 2;

        // VWAP'tan sapma (basis points)
        int64_t deviation_bps = ((int64_t)mid - (int64_t)current_vwap) * 10000 / (int64_t)current_vwap;

        if (config_.is_buy) {
            // ALIM: Fiyat VWAP'ın altındaysa al
            if (deviation_bps <= -(int64_t)config_.threshold_bps) {
                signal.should_trade = true;
                signal.quantity = std::min(config_.slice_size, config_.target_quantity - executed_quantity_);
                signal.limit_price = ask; // Agresif al
            }
        } else {
            // SATIM: Fiyat VWAP'ın üstündeyse sat
            if (deviation_bps >= (int64_t)config_.threshold_bps) {
                signal.should_trade = true;
                signal.quantity = std::min(config_.slice_size, config_.target_quantity - executed_quantity_);
                signal.limit_price = bid; // Agresif sat
            }
        }

        return signal;
    }

    // Fill geldiğinde çağır
    void on_fill(Quantity qty) { executed_quantity_ += qty; }

    // Durum sorgulama
    Quantity executed() const { return executed_quantity_; }
    Quantity remaining() const {
        return config_.target_quantity > executed_quantity_ ? config_.target_quantity - executed_quantity_ : 0;
    }
    bool is_complete() const { return executed_quantity_ >= config_.target_quantity; }
    double fill_rate() const {
        return config_.target_quantity > 0 ? (double)executed_quantity_ / config_.target_quantity : 0.0;
    }

    const VWAPConfig& config() const { return config_; }

    void reset() {
        cumulative_pv_ = 0;
        cumulative_volume_ = 0;
        executed_quantity_ = 0;
    }

private:
    VWAPConfig config_;
    uint64_t cumulative_pv_;
    Quantity cumulative_volume_;
    Quantity executed_quantity_;
};

} // namespace strategy
} // namespace hft
