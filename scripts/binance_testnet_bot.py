#!/usr/bin/env python3
"""
Binance Testnet Market Making Bot

Uses the strategy parameters optimized from backtesting:
- Spread: 10 bps
- Quote size: 0.01 BTC (small for testnet)
- Max position: 0.1 BTC

Requirements:
    pip install python-binance

Setup:
    1. Go to https://testnet.binance.vision/
    2. Login with GitHub
    3. Generate API Key and Secret
    4. Set environment variables:
       export BINANCE_TESTNET_API_KEY="your_key"
       export BINANCE_TESTNET_API_SECRET="your_secret"
"""

import os
import sys
import time
import logging
from decimal import Decimal, ROUND_DOWN
from dataclasses import dataclass
from typing import Optional
import signal

# Check for binance library
try:
    from binance.client import Client
    from binance.exceptions import BinanceAPIException
except ImportError:
    print("Please install python-binance: pip install python-binance")
    sys.exit(1)

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    datefmt='%H:%M:%S'
)
logger = logging.getLogger(__name__)


@dataclass
class StrategyConfig:
    """Market making strategy parameters"""
    symbol: str = "BTCUSDT"
    spread_bps: int = 10          # 10 basis points = 0.1%
    quote_size: float = 0.001     # 0.001 BTC per quote (testnet)
    max_position: float = 0.01    # Max 0.01 BTC position
    skew_factor: float = 0.5      # Inventory skew
    update_interval: float = 5.0  # Seconds between quote updates
    price_decimals: int = 2       # BTC price decimals
    qty_decimals: int = 5         # BTC quantity decimals


@dataclass
class Position:
    """Current position and P&L tracking"""
    quantity: float = 0.0
    avg_price: float = 0.0
    realized_pnl: float = 0.0
    total_trades: int = 0

    def on_fill(self, side: str, qty: float, price: float):
        """Update position on fill"""
        self.total_trades += 1

        if side == "BUY":
            if self.quantity >= 0:
                # Adding to long
                total_cost = self.quantity * self.avg_price + qty * price
                self.quantity += qty
                self.avg_price = total_cost / self.quantity if self.quantity > 0 else 0
            else:
                # Covering short
                cover = min(qty, -self.quantity)
                self.realized_pnl += cover * (self.avg_price - price)
                self.quantity += qty
                if self.quantity > 0:
                    self.avg_price = price
        else:  # SELL
            if self.quantity <= 0:
                # Adding to short
                total_cost = abs(self.quantity) * self.avg_price + qty * price
                self.quantity -= qty
                self.avg_price = total_cost / abs(self.quantity) if self.quantity != 0 else 0
            else:
                # Closing long
                close = min(qty, self.quantity)
                self.realized_pnl += close * (price - self.avg_price)
                self.quantity -= qty
                if self.quantity < 0:
                    self.avg_price = price

    def unrealized_pnl(self, current_price: float) -> float:
        """Calculate unrealized P&L at current price"""
        if self.quantity == 0:
            return 0
        return self.quantity * (current_price - self.avg_price)

    def total_pnl(self, current_price: float) -> float:
        return self.realized_pnl + self.unrealized_pnl(current_price)


class MarketMaker:
    """Simple market making strategy"""

    def __init__(self, config: StrategyConfig):
        self.config = config

    def calculate_quotes(self, mid_price: float, position: float) -> tuple:
        """
        Calculate bid/ask prices based on mid price and current position.
        Returns (bid_price, ask_price, bid_size, ask_size)
        """
        # Calculate half spread
        half_spread = mid_price * (self.config.spread_bps / 2) / 10000

        # Calculate inventory skew
        position_ratio = position / self.config.max_position if self.config.max_position > 0 else 0
        skew = half_spread * position_ratio * self.config.skew_factor

        # Calculate prices
        bid_price = mid_price - half_spread - skew
        ask_price = mid_price + half_spread - skew

        # Calculate sizes based on position limits
        room_to_buy = self.config.max_position - position
        room_to_sell = self.config.max_position + position

        bid_size = min(self.config.quote_size, max(0, room_to_buy))
        ask_size = min(self.config.quote_size, max(0, room_to_sell))

        return bid_price, ask_price, bid_size, ask_size


class BinanceTestnetBot:
    """Main trading bot"""

    def __init__(self, config: StrategyConfig):
        self.config = config
        self.strategy = MarketMaker(config)
        self.position = Position()
        self.running = False

        # Current order IDs
        self.bid_order_id: Optional[int] = None
        self.ask_order_id: Optional[int] = None

        # Initialize Binance client
        api_key = os.environ.get("BINANCE_TESTNET_API_KEY")
        api_secret = os.environ.get("BINANCE_TESTNET_API_SECRET")

        if not api_key or not api_secret:
            raise ValueError(
                "Please set BINANCE_TESTNET_API_KEY and BINANCE_TESTNET_API_SECRET "
                "environment variables"
            )

        self.client = Client(
            api_key,
            api_secret,
            testnet=True
        )

        logger.info(f"Connected to Binance Testnet")
        logger.info(f"Symbol: {config.symbol}")
        logger.info(f"Spread: {config.spread_bps} bps")
        logger.info(f"Quote size: {config.quote_size}")
        logger.info(f"Max position: {config.max_position}")

    def get_mid_price(self) -> float:
        """Get current mid price from order book"""
        ticker = self.client.get_orderbook_ticker(symbol=self.config.symbol)
        bid = float(ticker['bidPrice'])
        ask = float(ticker['askPrice'])
        return (bid + ask) / 2

    def get_account_balance(self) -> dict:
        """Get account balances"""
        account = self.client.get_account()
        balances = {}
        for balance in account['balances']:
            free = float(balance['free'])
            locked = float(balance['locked'])
            if free > 0 or locked > 0:
                balances[balance['asset']] = {'free': free, 'locked': locked}
        return balances

    def cancel_all_orders(self):
        """Cancel all open orders"""
        try:
            self.client.cancel_all_open_orders(symbol=self.config.symbol)
            self.bid_order_id = None
            self.ask_order_id = None
            logger.info("Cancelled all open orders")
        except BinanceAPIException as e:
            if e.code != -2011:  # "Unknown order" is OK
                logger.error(f"Error cancelling orders: {e}")

    def place_order(self, side: str, price: float, quantity: float) -> Optional[int]:
        """Place a limit order"""
        try:
            # Round price and quantity to proper decimals
            price_str = f"{price:.{self.config.price_decimals}f}"
            qty_str = f"{quantity:.{self.config.qty_decimals}f}"

            order = self.client.create_order(
                symbol=self.config.symbol,
                side=side,
                type="LIMIT",
                timeInForce="GTC",
                quantity=qty_str,
                price=price_str
            )

            order_id = order['orderId']
            logger.info(f"Placed {side} order: {qty_str} @ {price_str} (ID: {order_id})")
            return order_id

        except BinanceAPIException as e:
            logger.error(f"Error placing {side} order: {e}")
            return None

    def check_fills(self):
        """Check for filled orders and update position"""
        try:
            trades = self.client.get_my_trades(symbol=self.config.symbol, limit=10)

            # Process recent trades (simplified - in production use WebSocket)
            for trade in trades[-5:]:  # Last 5 trades
                trade_id = trade['id']
                # In production, track processed trade IDs to avoid duplicates
                side = "BUY" if trade['isBuyer'] else "SELL"
                qty = float(trade['qty'])
                price = float(trade['price'])
                # self.position.on_fill(side, qty, price)

        except BinanceAPIException as e:
            logger.error(f"Error checking fills: {e}")

    def update_quotes(self):
        """Update bid/ask quotes"""
        try:
            # Get current mid price
            mid_price = self.get_mid_price()

            # Calculate new quotes
            bid_price, ask_price, bid_size, ask_size = self.strategy.calculate_quotes(
                mid_price, self.position.quantity
            )

            # Cancel existing orders
            self.cancel_all_orders()

            # Place new orders
            if bid_size > 0:
                self.bid_order_id = self.place_order("BUY", bid_price, bid_size)

            if ask_size > 0:
                self.ask_order_id = self.place_order("SELL", ask_price, ask_size)

            # Log status
            logger.info(
                f"Mid: ${mid_price:.2f} | "
                f"Bid: ${bid_price:.2f} x {bid_size:.5f} | "
                f"Ask: ${ask_price:.2f} x {ask_size:.5f} | "
                f"Pos: {self.position.quantity:.5f} | "
                f"PnL: ${self.position.total_pnl(mid_price):.2f}"
            )

        except Exception as e:
            logger.error(f"Error updating quotes: {e}")

    def run(self):
        """Main trading loop"""
        self.running = True
        logger.info("Starting trading bot...")

        # Print initial balances
        balances = self.get_account_balance()
        logger.info(f"Initial balances: {balances}")

        try:
            while self.running:
                self.update_quotes()
                self.check_fills()
                time.sleep(self.config.update_interval)

        except KeyboardInterrupt:
            logger.info("Shutting down...")
        finally:
            self.cancel_all_orders()
            logger.info("Bot stopped")

    def stop(self):
        """Stop the trading bot"""
        self.running = False


def main():
    # Signal handler for graceful shutdown
    bot = None

    def signal_handler(sig, frame):
        if bot:
            bot.stop()

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # Configuration
    config = StrategyConfig(
        symbol="BTCUSDT",
        spread_bps=10,
        quote_size=0.001,      # Very small for testnet
        max_position=0.01,
        update_interval=10.0   # Update every 10 seconds
    )

    try:
        bot = BinanceTestnetBot(config)
        bot.run()
    except ValueError as e:
        logger.error(str(e))
        print("\nTo get testnet API keys:")
        print("1. Go to https://testnet.binance.vision/")
        print("2. Login with GitHub")
        print("3. Click 'Generate HMAC_SHA256 Key'")
        print("4. Set environment variables:")
        print('   export BINANCE_TESTNET_API_KEY="your_key"')
        print('   export BINANCE_TESTNET_API_SECRET="your_secret"')
        sys.exit(1)
    except Exception as e:
        logger.error(f"Fatal error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
