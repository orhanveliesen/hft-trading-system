/**
 * Order Book Testbench
 *
 * Verifies:
 * - Order add functionality
 * - Order cancel functionality
 * - Best bid/ask tracking
 * - Quantity updates
 */

`timescale 1ns / 1ps

module orderbook_tb;

    // Parameters
    parameter PRICE_WIDTH = 32;
    parameter QTY_WIDTH = 32;
    parameter ORDER_ID_WIDTH = 64;
    parameter MAX_PRICE_LEVELS = 256;

    // Clock and reset
    reg clk;
    reg rst_n;

    // Add interface
    reg                         add_valid;
    wire                        add_ready;
    reg  [ORDER_ID_WIDTH-1:0]   add_order_id;
    reg                         add_side;
    reg  [PRICE_WIDTH-1:0]      add_price;
    reg  [QTY_WIDTH-1:0]        add_quantity;

    // Cancel interface
    reg                         cancel_valid;
    wire                        cancel_ready;
    reg  [ORDER_ID_WIDTH-1:0]   cancel_order_id;
    wire                        cancel_found;

    // Execute interface
    reg                         exec_valid;
    wire                        exec_ready;
    reg  [ORDER_ID_WIDTH-1:0]   exec_order_id;
    reg  [QTY_WIDTH-1:0]        exec_quantity;
    wire                        exec_found;

    // BBO outputs
    wire [PRICE_WIDTH-1:0]      best_bid;
    wire [PRICE_WIDTH-1:0]      best_ask;
    wire [QTY_WIDTH-1:0]        best_bid_qty;
    wire [QTY_WIDTH-1:0]        best_ask_qty;
    wire                        best_bid_valid;
    wire                        best_ask_valid;

    // Status
    wire [15:0]                 total_bid_levels;
    wire [15:0]                 total_ask_levels;

    // Test counters
    integer test_num;
    integer pass_count;
    integer fail_count;

    // DUT
    orderbook #(
        .PRICE_WIDTH(PRICE_WIDTH),
        .QTY_WIDTH(QTY_WIDTH),
        .ORDER_ID_WIDTH(ORDER_ID_WIDTH),
        .MAX_PRICE_LEVELS(MAX_PRICE_LEVELS)
    ) dut (
        .clk(clk),
        .rst_n(rst_n),

        .add_valid(add_valid),
        .add_ready(add_ready),
        .add_order_id(add_order_id),
        .add_side(add_side),
        .add_price(add_price),
        .add_quantity(add_quantity),

        .cancel_valid(cancel_valid),
        .cancel_ready(cancel_ready),
        .cancel_order_id(cancel_order_id),
        .cancel_found(cancel_found),

        .exec_valid(exec_valid),
        .exec_ready(exec_ready),
        .exec_order_id(exec_order_id),
        .exec_quantity(exec_quantity),
        .exec_found(exec_found),

        .best_bid(best_bid),
        .best_ask(best_ask),
        .best_bid_qty(best_bid_qty),
        .best_ask_qty(best_ask_qty),
        .best_bid_valid(best_bid_valid),
        .best_ask_valid(best_ask_valid),

        .total_bid_levels(total_bid_levels),
        .total_ask_levels(total_ask_levels)
    );

    // Clock generation: 100 MHz
    initial clk = 0;
    always #5 clk = ~clk;

    // Test tasks
    task reset_dut;
    begin
        rst_n = 0;
        add_valid = 0;
        cancel_valid = 0;
        exec_valid = 0;
        #20;
        rst_n = 1;
        #20;
    end
    endtask

    task add_order;
        input [ORDER_ID_WIDTH-1:0] id;
        input side;
        input [PRICE_WIDTH-1:0] price;
        input [QTY_WIDTH-1:0] qty;
    begin
        @(posedge clk);
        add_valid = 1;
        add_order_id = id;
        add_side = side;
        add_price = price;
        add_quantity = qty;

        // Wait for ready
        while (!add_ready) @(posedge clk);
        @(posedge clk);
        add_valid = 0;

        // Wait for completion
        #50;
    end
    endtask

    task cancel_order;
        input [ORDER_ID_WIDTH-1:0] id;
    begin
        @(posedge clk);
        cancel_valid = 1;
        cancel_order_id = id;

        while (!cancel_ready) @(posedge clk);
        @(posedge clk);
        cancel_valid = 0;

        #50;
    end
    endtask

    task execute_order;
        input [ORDER_ID_WIDTH-1:0] id;
        input [QTY_WIDTH-1:0] qty;
    begin
        @(posedge clk);
        exec_valid = 1;
        exec_order_id = id;
        exec_quantity = qty;

        while (!exec_ready) @(posedge clk);
        @(posedge clk);
        exec_valid = 0;

        #50;
    end
    endtask

    task check_result;
        input [255:0] test_name;
        input condition;
    begin
        test_num = test_num + 1;
        if (condition) begin
            $display("PASS: Test %0d - %s", test_num, test_name);
            pass_count = pass_count + 1;
        end
        else begin
            $display("FAIL: Test %0d - %s", test_num, test_name);
            fail_count = fail_count + 1;
        end
    end
    endtask

    // Main test sequence
    initial begin
        $display("========================================");
        $display("HFT Order Book Testbench");
        $display("========================================");

        test_num = 0;
        pass_count = 0;
        fail_count = 0;

        // Initialize
        reset_dut();

        // Test 1: Empty book should have no valid BBO
        check_result("Empty book - no valid bid", !best_bid_valid);
        check_result("Empty book - no valid ask", !best_ask_valid);

        // Test 2: Add a buy order
        add_order(1, 0, 10000, 100);  // ID=1, Buy, Price=10000, Qty=100
        check_result("Add buy - valid bid", best_bid_valid);
        check_result("Add buy - correct bid price", best_bid == 10000);
        check_result("Add buy - correct bid qty", best_bid_qty == 100);
        check_result("Add buy - no ask", !best_ask_valid);

        // Test 3: Add a sell order
        add_order(2, 1, 10100, 50);   // ID=2, Sell, Price=10100, Qty=50
        check_result("Add sell - valid ask", best_ask_valid);
        check_result("Add sell - correct ask price", best_ask == 10100);
        check_result("Add sell - correct ask qty", best_ask_qty == 50);

        // Test 4: Add another buy at same price
        add_order(3, 0, 10000, 200);  // ID=3, Buy, Price=10000, Qty=200
        check_result("Add same price - aggregated qty", best_bid_qty == 300);

        // Test 5: Add higher bid
        add_order(4, 0, 10050, 75);   // ID=4, Buy, Price=10050, Qty=75
        check_result("Higher bid - new best bid", best_bid == 10050);
        check_result("Higher bid - new best qty", best_bid_qty == 75);

        // Test 6: Add lower ask
        add_order(5, 1, 10080, 25);   // ID=5, Sell, Price=10080, Qty=25
        check_result("Lower ask - new best ask", best_ask == 10080);
        check_result("Lower ask - new best qty", best_ask_qty == 25);

        // Test 7: Cancel an order
        cancel_order(4);  // Cancel the best bid
        check_result("Cancel - order found", cancel_found);
        check_result("Cancel - bid reverts", best_bid == 10000);

        // Test 8: Partial execution
        execute_order(1, 30);  // Execute 30 of order 1 (100 qty)
        check_result("Partial exec - found", exec_found);
        // Quantity should be reduced

        // Test 9: Full execution
        execute_order(5, 25);  // Execute all of order 5
        check_result("Full exec - ask changes", best_ask == 10100);

        // Test 10: Cancel non-existent order
        cancel_order(999);
        check_result("Cancel non-existent", !cancel_found);

        // Summary
        $display("========================================");
        $display("Test Summary: %0d passed, %0d failed", pass_count, fail_count);
        $display("========================================");

        if (fail_count == 0)
            $display("ALL TESTS PASSED!");
        else
            $display("SOME TESTS FAILED!");

        #100;
        $finish;
    end

    // Timeout watchdog
    initial begin
        #100000;
        $display("ERROR: Simulation timeout!");
        $finish;
    end

    // VCD dump for waveform viewing
    initial begin
        $dumpfile("orderbook_tb.vcd");
        $dumpvars(0, orderbook_tb);
    end

endmodule
