/**
 * HFT Top-Level Module
 *
 * Integrates:
 * - Matching engine with order book
 * - Market data input parsing
 * - Trade output formatting
 * - AXI-Stream interfaces for connectivity
 *
 * Target: Xilinx/Intel FPGAs, 100+ MHz operation
 */

module hft_top #(
    parameter PRICE_WIDTH = 32,
    parameter QTY_WIDTH = 32,
    parameter ORDER_ID_WIDTH = 64,
    parameter TRADER_ID_WIDTH = 32,
    parameter MAX_PRICE_LEVELS = 256,
    parameter DATA_WIDTH = 128        // AXI-Stream data width
) (
    input  wire                         clk,
    input  wire                         rst_n,

    // ========================================
    // AXI-Stream Slave: Order Input
    // ========================================
    input  wire [DATA_WIDTH-1:0]        s_axis_order_tdata,
    input  wire                         s_axis_order_tvalid,
    output wire                         s_axis_order_tready,

    // ========================================
    // AXI-Stream Master: Trade Output
    // ========================================
    output reg  [DATA_WIDTH-1:0]        m_axis_trade_tdata,
    output reg                          m_axis_trade_tvalid,
    input  wire                         m_axis_trade_tready,
    output reg                          m_axis_trade_tlast,

    // ========================================
    // BBO Output (direct wires for low latency)
    // ========================================
    output wire [PRICE_WIDTH-1:0]       best_bid,
    output wire [PRICE_WIDTH-1:0]       best_ask,
    output wire [QTY_WIDTH-1:0]         best_bid_qty,
    output wire [QTY_WIDTH-1:0]         best_ask_qty,
    output wire                         best_bid_valid,
    output wire                         best_ask_valid,

    // ========================================
    // Status and Statistics
    // ========================================
    output wire [31:0]                  total_trades,
    output wire [63:0]                  total_volume,
    output wire [31:0]                  orders_processed,
    output wire                         engine_ready,

    // ========================================
    // Debug/Monitor
    // ========================================
    output wire [3:0]                   state_debug
);

    // ========================================
    // Order Parsing
    // ========================================

    // Order input format (128-bit packed):
    // [127:124] - Message type (0=New, 1=Cancel)
    // [123:92]  - Price (32 bits)
    // [91:60]   - Quantity (32 bits)
    // [59:28]   - Trader ID (32 bits)
    // [27:1]    - Reserved
    // [0]       - Side (0=Buy, 1=Sell)
    //
    // Order ID is auto-generated internally

    wire [3:0]                  msg_type = s_axis_order_tdata[127:124];
    wire [PRICE_WIDTH-1:0]      parsed_price = s_axis_order_tdata[123:92];
    wire [QTY_WIDTH-1:0]        parsed_qty = s_axis_order_tdata[91:60];
    wire [TRADER_ID_WIDTH-1:0]  parsed_trader = s_axis_order_tdata[59:28];
    wire                        parsed_side = s_axis_order_tdata[0];

    // Order ID counter
    reg [ORDER_ID_WIDTH-1:0]    order_id_counter;

    // ========================================
    // Matching Engine Instance
    // ========================================

    reg                         me_order_valid;
    wire                        me_order_ready;
    reg  [ORDER_ID_WIDTH-1:0]   me_order_id;
    reg  [TRADER_ID_WIDTH-1:0]  me_trader_id;
    reg                         me_order_side;
    reg  [PRICE_WIDTH-1:0]      me_order_price;
    reg  [QTY_WIDTH-1:0]        me_order_quantity;

    reg                         me_cancel_valid;
    wire                        me_cancel_ready;
    reg  [ORDER_ID_WIDTH-1:0]   me_cancel_order_id;
    wire                        me_cancel_success;

    wire                        me_trade_valid;
    reg                         me_trade_ready;
    wire [ORDER_ID_WIDTH-1:0]   me_trade_aggressor_id;
    wire [ORDER_ID_WIDTH-1:0]   me_trade_passive_id;
    wire [PRICE_WIDTH-1:0]      me_trade_price;
    wire [QTY_WIDTH-1:0]        me_trade_quantity;
    wire                        me_trade_aggressor_side;

    matching_engine #(
        .PRICE_WIDTH(PRICE_WIDTH),
        .QTY_WIDTH(QTY_WIDTH),
        .ORDER_ID_WIDTH(ORDER_ID_WIDTH),
        .TRADER_ID_WIDTH(TRADER_ID_WIDTH),
        .MAX_PRICE_LEVELS(MAX_PRICE_LEVELS)
    ) matching_engine_inst (
        .clk(clk),
        .rst_n(rst_n),

        .order_valid(me_order_valid),
        .order_ready(me_order_ready),
        .order_id(me_order_id),
        .trader_id(me_trader_id),
        .order_side(me_order_side),
        .order_price(me_order_price),
        .order_quantity(me_order_quantity),

        .cancel_valid(me_cancel_valid),
        .cancel_ready(me_cancel_ready),
        .cancel_order_id(me_cancel_order_id),
        .cancel_success(me_cancel_success),

        .trade_valid(me_trade_valid),
        .trade_ready(me_trade_ready),
        .trade_aggressor_id(me_trade_aggressor_id),
        .trade_passive_id(me_trade_passive_id),
        .trade_price(me_trade_price),
        .trade_quantity(me_trade_quantity),
        .trade_aggressor_side(me_trade_aggressor_side),

        .best_bid(best_bid),
        .best_ask(best_ask),
        .best_bid_qty(best_bid_qty),
        .best_ask_qty(best_ask_qty),
        .best_bid_valid(best_bid_valid),
        .best_ask_valid(best_ask_valid),

        .total_trades(total_trades),
        .total_volume(total_volume)
    );

    // ========================================
    // State Machine
    // ========================================

    localparam ST_IDLE      = 3'd0;
    localparam ST_PARSE     = 3'd1;
    localparam ST_SUBMIT    = 3'd2;
    localparam ST_WAIT      = 3'd3;
    localparam ST_TRADE_OUT = 3'd4;

    reg [2:0] state;
    reg [31:0] orders_count;

    assign state_debug = {1'b0, state};
    assign orders_processed = orders_count;
    assign engine_ready = (state == ST_IDLE) && me_order_ready && me_cancel_ready;

    // Input ready when idle and engine ready
    assign s_axis_order_tready = (state == ST_IDLE) && me_order_ready;

    // ========================================
    // Main Logic
    // ========================================

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= ST_IDLE;
            order_id_counter <= 64'd1;
            orders_count <= 0;

            me_order_valid <= 1'b0;
            me_cancel_valid <= 1'b0;
            me_trade_ready <= 1'b1;

            m_axis_trade_tdata <= 0;
            m_axis_trade_tvalid <= 1'b0;
            m_axis_trade_tlast <= 1'b0;
        end
        else begin
            // Default: deassert
            me_order_valid <= 1'b0;
            me_cancel_valid <= 1'b0;

            case (state)
                ST_IDLE: begin
                    m_axis_trade_tvalid <= 1'b0;

                    if (s_axis_order_tvalid && s_axis_order_tready) begin
                        state <= ST_PARSE;
                    end

                    // Also check for trade output
                    if (me_trade_valid) begin
                        state <= ST_TRADE_OUT;
                    end
                end

                ST_PARSE: begin
                    // Parse the input message
                    if (msg_type == 4'd0) begin
                        // New order
                        me_order_id <= order_id_counter;
                        me_trader_id <= parsed_trader;
                        me_order_side <= parsed_side;
                        me_order_price <= parsed_price;
                        me_order_quantity <= parsed_qty;

                        order_id_counter <= order_id_counter + 1;
                        orders_count <= orders_count + 1;

                        state <= ST_SUBMIT;
                    end
                    else if (msg_type == 4'd1) begin
                        // Cancel order (order ID in lower bits)
                        me_cancel_order_id <= s_axis_order_tdata[ORDER_ID_WIDTH-1:0];
                        me_cancel_valid <= 1'b1;
                        state <= ST_WAIT;
                    end
                    else begin
                        // Unknown message type, ignore
                        state <= ST_IDLE;
                    end
                end

                ST_SUBMIT: begin
                    me_order_valid <= 1'b1;
                    state <= ST_WAIT;
                end

                ST_WAIT: begin
                    // Wait for operation to complete
                    if ((me_order_valid && me_order_ready) ||
                        (me_cancel_valid && me_cancel_ready)) begin
                        state <= ST_IDLE;
                    end
                end

                ST_TRADE_OUT: begin
                    // Format and output trade
                    // Trade format (128-bit):
                    // [127:96] - Trade price
                    // [95:64]  - Trade quantity
                    // [63:0]   - Aggressor order ID
                    m_axis_trade_tdata <= {
                        me_trade_price,
                        me_trade_quantity,
                        me_trade_aggressor_id
                    };
                    m_axis_trade_tvalid <= 1'b1;
                    m_axis_trade_tlast <= 1'b1;

                    if (m_axis_trade_tready) begin
                        me_trade_ready <= 1'b1;
                        state <= ST_IDLE;
                    end
                end

                default: state <= ST_IDLE;
            endcase
        end
    end

endmodule
