#pragma once

#include "../types.hpp"

namespace hft {
namespace strategy {

/**
 * Fair Value Strategy
 *
 * Mantık: Teorik "doğru" fiyatı hesapla, sapmalarda trade et
 *
 * Fair Value hesaplama yöntemleri:
 *   1. Micro-price: Bid/Ask ağırlıklı ortalama
 *   2. Index-based: Futures'tan spot'u türet
 *   3. Multi-source: Birden fazla kaynağı birleştir
 *
 *   Market Price < Fair Value - threshold → AL
 *   Market Price > Fair Value + threshold → SAT
 */

enum class FVSignal : uint8_t {
    Hold = 0,
    Buy  = 1,
    Sell = 2
};

struct FairValueConfig {
    uint32_t threshold_bps = 3;      // Fair value'dan sapma eşiği (bps)
    double ema_alpha = 0.1;          // EMA smoothing factor
    Quantity order_size = 100;
    int64_t max_position = 1000;
    bool use_microprice = true;      // Micro-price kullan
};

class FairValueStrategy {
public:
    explicit FairValueStrategy(const FairValueConfig& config = {})
        : config_(config)
        , fair_value_ema_(0.0)
        , initialized_(false)
    {}

    // Micro-price hesapla
    // Bid/ask size'a göre ağırlıklı ortalama
    // Daha fazla bid varsa, fiyat bid'e yakın (alıcı baskısı)
    static double microprice(Price bid, Price ask, Quantity bid_size, Quantity ask_size) {
        if (bid_size + ask_size == 0) {
            return (double)(bid + ask) / 2.0;
        }
        // Microprice = (bid × ask_size + ask × bid_size) / (bid_size + ask_size)
        return ((double)bid * ask_size + (double)ask * bid_size) / (bid_size + ask_size);
    }

    // Fair value güncelle
    void update_fair_value(double new_value) {
        if (!initialized_) {
            fair_value_ema_ = new_value;
            initialized_ = true;
        } else {
            // Exponential Moving Average
            fair_value_ema_ = config_.ema_alpha * new_value +
                              (1.0 - config_.ema_alpha) * fair_value_ema_;
        }
    }

    // Sinyal üret
    FVSignal operator()(Price bid, Price ask, Quantity bid_size, Quantity ask_size,
                        int64_t position) {
        if (bid == INVALID_PRICE || ask == INVALID_PRICE || bid >= ask) {
            return FVSignal::Hold;
        }

        // Fair value hesapla ve güncelle
        double fv;
        if (config_.use_microprice) {
            fv = microprice(bid, ask, bid_size, ask_size);
        } else {
            fv = (double)(bid + ask) / 2.0;
        }
        update_fair_value(fv);

        // Market mid price
        double mid = (double)(bid + ask) / 2.0;

        // Fair value'dan sapma (basis points)
        double deviation_bps = (mid - fair_value_ema_) * 10000.0 / fair_value_ema_;

        // Sinyal üret
        if (deviation_bps < -(double)config_.threshold_bps) {
            // Market fiyatı fair value'nun altında → AL
            if (position < config_.max_position) {
                return FVSignal::Buy;
            }
        } else if (deviation_bps > (double)config_.threshold_bps) {
            // Market fiyatı fair value'nun üstünde → SAT
            if (position > -config_.max_position) {
                return FVSignal::Sell;
            }
        }

        return FVSignal::Hold;
    }

    // Harici fair value ile çalış (örn: futures'tan türetilmiş)
    FVSignal with_external_fv(Price bid, Price ask, double external_fv, int64_t position) {
        if (bid == INVALID_PRICE || ask == INVALID_PRICE || bid >= ask) {
            return FVSignal::Hold;
        }

        update_fair_value(external_fv);

        double mid = (double)(bid + ask) / 2.0;
        double deviation_bps = (mid - fair_value_ema_) * 10000.0 / fair_value_ema_;

        if (deviation_bps < -(double)config_.threshold_bps) {
            if (position < config_.max_position) {
                return FVSignal::Buy;
            }
        } else if (deviation_bps > (double)config_.threshold_bps) {
            if (position > -config_.max_position) {
                return FVSignal::Sell;
            }
        }

        return FVSignal::Hold;
    }

    // Durum sorgulama
    double fair_value() const { return fair_value_ema_; }
    bool is_initialized() const { return initialized_; }

    const FairValueConfig& config() const { return config_; }

    void reset() {
        fair_value_ema_ = 0.0;
        initialized_ = false;
    }

private:
    FairValueConfig config_;
    double fair_value_ema_;
    bool initialized_;
};

/**
 * Index Arbitrage Strategy
 *
 * Özel durum: Index futures ile spot basket arasında arbitraj
 *
 * SPY spot = f(ES futures) teorik olarak
 * Sapma varsa arbitraj fırsatı
 */
struct IndexArbConfig {
    double futures_multiplier = 1.0;  // Futures → Spot dönüşüm çarpanı
    double cost_of_carry_bps = 5;     // Taşıma maliyeti
    uint32_t threshold_bps = 2;       // Arbitraj eşiği
    Quantity order_size = 100;
};

class IndexArbitrage {
public:
    explicit IndexArbitrage(const IndexArbConfig& config = {})
        : config_(config)
    {}

    // Futures fiyatından teorik spot hesapla
    Price theoretical_spot(Price futures_price) const {
        // Spot = Futures × multiplier - carry cost
        double spot = (double)futures_price * config_.futures_multiplier;
        spot *= (1.0 - config_.cost_of_carry_bps / 10000.0);
        return static_cast<Price>(spot);
    }

    // Sinyal üret
    FVSignal operator()(Price spot_bid, Price spot_ask, Price futures_price) {
        if (spot_bid == INVALID_PRICE || spot_ask == INVALID_PRICE ||
            futures_price == INVALID_PRICE) {
            return FVSignal::Hold;
        }

        Price theo = theoretical_spot(futures_price);
        Price mid = (spot_bid + spot_ask) / 2;

        int64_t deviation_bps = ((int64_t)mid - (int64_t)theo) * 10000 / (int64_t)theo;

        if (deviation_bps < -(int64_t)config_.threshold_bps) {
            // Spot ucuz, futures'a göre → AL spot
            return FVSignal::Buy;
        } else if (deviation_bps > (int64_t)config_.threshold_bps) {
            // Spot pahalı, futures'a göre → SAT spot
            return FVSignal::Sell;
        }

        return FVSignal::Hold;
    }

    const IndexArbConfig& config() const { return config_; }

private:
    IndexArbConfig config_;
};

}  // namespace strategy
}  // namespace hft
