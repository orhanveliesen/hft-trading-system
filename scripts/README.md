# Python Helper Scripts

Bu klasördeki Python scriptleri, ana C++ trading sistemi için yardımcı araçlardır.

## Kurulum

```bash
cd /path/to/hft
pip install -r requirements.txt
```

## Scriptler

### 1. download_binance_data.py

Binance'den tarihi kline (mum) verisi indirir. Backtest için kullanılır.

**Kullanım:**
```bash
python scripts/download_binance_data.py --symbol BTCUSDT --interval 1m --days 30
```

**Parametreler:**
- `--symbol`: Trading pair (örn: BTCUSDT, ETHUSDT)
- `--interval`: Kline interval (1m, 5m, 15m, 1h, 4h, 1d)
- `--days`: Kaç günlük veri indirileceği

**Çıktı:**
- `data/` klasörüne CSV dosyası olarak kaydeder
- Format: `{symbol}_{interval}.csv`

**C++ Entegrasyonu:**
- İndirilen veriler `tools/run_backtest` ile kullanılır
- `include/backtest/kline_backtest.hpp` CSV dosyalarını parse eder

---

### 2. binance_testnet_bot.py

Binance Testnet üzerinde strateji test botu. Gerçek para riski olmadan canlı ortam simülasyonu.

**Kurulum:**
1. [Binance Testnet](https://testnet.binance.vision/) hesabı oluştur
2. API key al
3. Environment variable'ları ayarla:
```bash
export BINANCE_TESTNET_API_KEY="your_api_key"
export BINANCE_TESTNET_SECRET="your_secret"
```

**Kullanım:**
```bash
python scripts/binance_testnet_bot.py --symbol BTCUSDT --strategy mean_reversion
```

**Özellikler:**
- WebSocket ile real-time market data
- REST API ile order gönderme
- Basit strateji implementasyonları
- P&L takibi

**Not:** Bu bot test amaçlıdır. Production için C++ implementasyonunu kullanın.

---

## Dosya Yapısı

```
scripts/
├── README.md                  # Bu dosya
├── download_binance_data.py   # Veri indirme
└── binance_testnet_bot.py     # Testnet bot

requirements.txt               # Python bağımlılıkları (proje kökünde)
```

## C++ Projesi ile İlişki

```
┌─────────────────────────────────────────────────────────────┐
│                    Python Scripts                            │
│  ┌─────────────────────┐    ┌─────────────────────────┐    │
│  │ download_binance_   │    │ binance_testnet_bot.py │    │
│  │ data.py             │    │ (strateji testi)       │    │
│  └──────────┬──────────┘    └─────────────────────────┘    │
│             │                                               │
│             ▼                                               │
│  ┌─────────────────────┐                                   │
│  │  data/*.csv         │                                   │
│  │  (kline verileri)   │                                   │
│  └──────────┬──────────┘                                   │
└─────────────┼───────────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────────────────────────┐
│                      C++ Project                             │
│  ┌─────────────────────┐    ┌─────────────────────────┐    │
│  │ tools/run_backtest  │    │ include/backtest/       │    │
│  │ (backtest runner)   │◄───│ kline_backtest.hpp      │    │
│  └─────────────────────┘    └─────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```
