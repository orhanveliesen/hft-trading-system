#pragma once

#include "../types.hpp"

namespace hft {
namespace strategy {

/**
 * Simple Mean Reversion - HFT'nin "Hello World"u
 *
 * Mantık:
 *   - Fiyat düştü → Al (geri çıkacak beklentisi)
 *   - Fiyat çıktı → Sat (geri düşecek beklentisi)
 *
 * Bu strateji gerçek hayatta para kazanmaz, ama:
 *   - Tüm component'leri nasıl kullanacağını gösterir
 *   - Market data → Signal → Order akışını öğretir
 */

enum class Signal : uint8_t {
    Hold = 0,
    Buy  = 1,
    Sell = 2
};

struct SimpleMRConfig {
    Quantity order_size = 100;       // Her işlemde kaç lot
    int64_t max_position = 1000;     // Maksimum pozisyon
};

class SimpleMeanReversion {
public:
    explicit SimpleMeanReversion(const SimpleMRConfig& config = {})
        : config_(config)
        , last_mid_(INVALID_PRICE)
    {}

    // Ana fonksiyon: Market data geldi, ne yapayım?
    Signal operator()(Price bid, Price ask, int64_t current_position) {
        // Geçersiz veri kontrolü
        if (bid == INVALID_PRICE || ask == INVALID_PRICE || bid >= ask) {
            return Signal::Hold;
        }

        // Mid price hesapla
        Price mid = (bid + ask) / 2;

        // İlk tick - referans al
        if (last_mid_ == INVALID_PRICE) {
            last_mid_ = mid;
            return Signal::Hold;
        }

        // Fiyat değişmedi
        if (mid == last_mid_) {
            return Signal::Hold;
        }

        Signal signal = Signal::Hold;

        // Fiyat DÜŞTÜ → AL (mean reversion: geri çıkacak)
        if (mid < last_mid_) {
            if (can_buy(current_position)) {
                signal = Signal::Buy;
            }
        }
        // Fiyat ÇIKTI → SAT (mean reversion: geri düşecek)
        else if (mid > last_mid_) {
            if (can_sell(current_position)) {
                signal = Signal::Sell;
            }
        }

        // Referansı güncelle
        last_mid_ = mid;

        return signal;
    }

    // Overload: TopOfBook ile de çalışsın
    Signal operator()(Price bid, Price ask) {
        return (*this)(bid, ask, 0);  // Position bilgisi yoksa 0 varsay
    }

    // Config erişimi
    const SimpleMRConfig& config() const { return config_; }
    Quantity order_size() const { return config_.order_size; }

    // Reset (test için)
    void reset() { last_mid_ = INVALID_PRICE; }

private:
    bool can_buy(int64_t position) const {
        return position < config_.max_position;
    }

    bool can_sell(int64_t position) const {
        return position > -config_.max_position;
    }

    SimpleMRConfig config_;
    Price last_mid_;
};

}  // namespace strategy
}  // namespace hft
