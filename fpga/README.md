# HFT FPGA Implementation

This directory contains the FPGA RTL (Register Transfer Level) implementation of the HFT Order Book and Matching Engine in Verilog.

## Directory Structure

```
fpga/
├── rtl/              # Synthesizable Verilog modules
│   ├── orderbook.v   # Order book implementation
│   ├── matching_engine.v  # Price-time priority matching
│   └── hft_top.v     # Top-level integration module
├── tb/               # Testbenches
│   └── orderbook_tb.v    # Order book testbench
└── sim/              # Simulation files
    └── Makefile      # Build automation
```

## Modules

### 1. Order Book (`orderbook.v`)

Hardware implementation of a limit order book with:
- **O(1) BBO (Best Bid/Offer) lookup** via cached values
- **256 price levels per side** (configurable)
- **1024 order tracking** (CAM-like structure)
- **Operations**: Add, Cancel, Execute (partial fill)

**Parameters:**
- `PRICE_WIDTH`: 32 bits (fixed-point, 4 decimal places)
- `QTY_WIDTH`: 32 bits
- `ORDER_ID_WIDTH`: 64 bits
- `MAX_PRICE_LEVELS`: 256

**Interfaces:**
- Add Order: AXI-Stream style handshake
- Cancel Order: With success flag
- Execute Order: Partial fill support
- BBO Output: Low-latency direct wires

### 2. Matching Engine (`matching_engine.v`)

Price-time priority matching engine that wraps the order book:
- **Automatic matching** when orders cross
- **Trade generation** with aggressor/passive IDs
- **Statistics**: Trade count, volume
- **Integrated order book** instance

### 3. Top Module (`hft_top.v`)

Complete HFT system with:
- **AXI-Stream input** for orders (128-bit)
- **AXI-Stream output** for trades
- **Direct BBO wires** for minimal latency
- **Order ID generation**
- **Statistics counters**

## Simulation

### Prerequisites

```bash
# Install Icarus Verilog
sudo apt-get install iverilog

# Optional: GTKWave for waveform viewing
sudo apt-get install gtkwave
```

### Running Tests

```bash
cd fpga/sim
make orderbook_sim
```

Expected output:
```
========================================
HFT Order Book Testbench
========================================
PASS: Test 1 - Empty book - no valid bid
PASS: Test 2 - Empty book - no valid ask
...
ALL TESTS PASSED!
========================================
```

### Viewing Waveforms

```bash
make view   # Opens GTKWave with orderbook_tb.vcd
```

## FPGA Synthesis

### Target Platforms

The design is portable and can target:
- **Xilinx**: Artix-7, Kintex-7, Zynq, UltraScale
- **Intel/Altera**: Cyclone V, Arria 10, Stratix 10

### Resource Estimates (Artix-7)

| Module | LUTs | FFs | BRAM |
|--------|------|-----|------|
| orderbook | ~5K | ~3K | 4 |
| matching_engine | ~7K | ~4K | 4 |
| hft_top | ~8K | ~5K | 4 |

### Clock Frequency

- Target: **200+ MHz** on modern FPGAs
- Latency: **~5 cycles** for order-to-trade

## Integration Example

```verilog
hft_top #(
    .PRICE_WIDTH(32),
    .QTY_WIDTH(32),
    .ORDER_ID_WIDTH(64)
) hft_inst (
    .clk(sys_clk_200mhz),
    .rst_n(rst_n),

    // Order input from network
    .s_axis_order_tdata(order_data),
    .s_axis_order_tvalid(order_valid),
    .s_axis_order_tready(order_ready),

    // Trade output to network
    .m_axis_trade_tdata(trade_data),
    .m_axis_trade_tvalid(trade_valid),
    .m_axis_trade_tready(trade_ready),

    // BBO for strategy logic
    .best_bid(best_bid),
    .best_ask(best_ask),
    .best_bid_valid(bid_valid),
    .best_ask_valid(ask_valid)
);
```

## Order Format (128-bit)

```
[127:124] Message Type: 0=New Order, 1=Cancel
[123:92]  Price (32 bits, fixed-point)
[91:60]   Quantity (32 bits)
[59:28]   Trader ID (32 bits)
[27:1]    Reserved
[0]       Side: 0=Buy, 1=Sell
```

## Trade Format (128-bit)

```
[127:96]  Trade Price (32 bits)
[95:64]   Trade Quantity (32 bits)
[63:0]    Aggressor Order ID (64 bits)
```

## Learning Resources

1. **Verilog Basics**: The modules use standard Verilog-2001
2. **State Machines**: See matching_engine.v for FSM patterns
3. **AXI-Stream**: Industry-standard streaming protocol
4. **Clock Domain**: Single clock design for simplicity

## Future Enhancements

- [ ] Multi-symbol support
- [ ] Network packet parsing (UDP/ITCH)
- [ ] FIFO queues at each price level
- [ ] Pipelining for higher throughput
- [ ] Self-trade prevention with trader IDs
