use crate::types::*;
use std::collections::HashMap;

/// Price level containing all orders at a specific price
#[derive(Debug, Default)]
struct PriceLevel {
    price: Price,
    total_quantity: Quantity,
    orders: Vec<OrderId>,  // Order IDs at this level (FIFO)
}

/// HFT-grade Order Book
/// - Pre-allocated storage with HashMap for O(1) lookup
/// - Sorted price levels using BTreeMap-like approach
/// - O(1) best bid/ask via cached values
pub struct OrderBook {
    /// All orders indexed by ID
    orders: HashMap<OrderId, Order>,

    /// Bid levels sorted by price (descending)
    bid_levels: Vec<PriceLevel>,

    /// Ask levels sorted by price (ascending)
    ask_levels: Vec<PriceLevel>,

    /// Cached best prices for O(1) access
    best_bid: Option<Price>,
    best_ask: Option<Price>,
}

impl OrderBook {
    pub fn new() -> Self {
        Self {
            orders: HashMap::with_capacity(1_000_000),
            bid_levels: Vec::with_capacity(10_000),
            ask_levels: Vec::with_capacity(10_000),
            best_bid: None,
            best_ask: None,
        }
    }

    /// Add an order to the book
    pub fn add_order(&mut self, id: OrderId, side: Side, price: Price, quantity: Quantity) {
        let order = Order::new(id, side, price, quantity);
        self.orders.insert(id, order);

        match side {
            Side::Buy => self.add_to_bids(id, price, quantity),
            Side::Sell => self.add_to_asks(id, price, quantity),
        }
    }

    /// Cancel an order by ID
    pub fn cancel_order(&mut self, id: OrderId) -> bool {
        if let Some(order) = self.orders.remove(&id) {
            match order.side {
                Side::Buy => self.remove_from_bids(id, order.price, order.quantity),
                Side::Sell => self.remove_from_asks(id, order.price, order.quantity),
            }
            true
        } else {
            false
        }
    }

    /// Execute (partially or fully) an order
    pub fn execute_order(&mut self, id: OrderId, quantity: Quantity) -> bool {
        if let Some(order) = self.orders.get_mut(&id) {
            let exec_qty = quantity.min(order.quantity);
            let price = order.price;
            let side = order.side;

            if exec_qty >= order.quantity {
                // Full execution - remove order
                self.orders.remove(&id);
                match side {
                    Side::Buy => self.remove_from_bids(id, price, exec_qty),
                    Side::Sell => self.remove_from_asks(id, price, exec_qty),
                }
            } else {
                // Partial execution - reduce quantity
                order.quantity -= exec_qty;
                match side {
                    Side::Buy => self.reduce_bid_quantity(price, exec_qty),
                    Side::Sell => self.reduce_ask_quantity(price, exec_qty),
                }
            }
            true
        } else {
            false
        }
    }

    /// Get best bid price
    pub fn best_bid(&self) -> Price {
        self.best_bid.unwrap_or(INVALID_PRICE)
    }

    /// Get best ask price
    pub fn best_ask(&self) -> Price {
        self.best_ask.unwrap_or(INVALID_PRICE)
    }

    /// Get total quantity at a bid price
    pub fn bid_quantity_at(&self, price: Price) -> Quantity {
        self.bid_levels
            .iter()
            .find(|l| l.price == price)
            .map(|l| l.total_quantity)
            .unwrap_or(0)
    }

    /// Get total quantity at an ask price
    pub fn ask_quantity_at(&self, price: Price) -> Quantity {
        self.ask_levels
            .iter()
            .find(|l| l.price == price)
            .map(|l| l.total_quantity)
            .unwrap_or(0)
    }

    // === Private methods ===

    fn add_to_bids(&mut self, id: OrderId, price: Price, quantity: Quantity) {
        // Find or create level
        if let Some(level) = self.bid_levels.iter_mut().find(|l| l.price == price) {
            level.orders.push(id);
            level.total_quantity += quantity;
        } else {
            // Insert new level in sorted order (descending)
            let level = PriceLevel {
                price,
                total_quantity: quantity,
                orders: vec![id],
            };
            let pos = self.bid_levels.iter().position(|l| l.price < price).unwrap_or(self.bid_levels.len());
            self.bid_levels.insert(pos, level);
        }

        // Update best bid
        if self.best_bid.is_none() || price > self.best_bid.unwrap() {
            self.best_bid = Some(price);
        }
    }

    fn add_to_asks(&mut self, id: OrderId, price: Price, quantity: Quantity) {
        // Find or create level
        if let Some(level) = self.ask_levels.iter_mut().find(|l| l.price == price) {
            level.orders.push(id);
            level.total_quantity += quantity;
        } else {
            // Insert new level in sorted order (ascending)
            let level = PriceLevel {
                price,
                total_quantity: quantity,
                orders: vec![id],
            };
            let pos = self.ask_levels.iter().position(|l| l.price > price).unwrap_or(self.ask_levels.len());
            self.ask_levels.insert(pos, level);
        }

        // Update best ask
        if self.best_ask.is_none() || price < self.best_ask.unwrap() {
            self.best_ask = Some(price);
        }
    }

    fn remove_from_bids(&mut self, id: OrderId, price: Price, quantity: Quantity) {
        if let Some(pos) = self.bid_levels.iter().position(|l| l.price == price) {
            let level = &mut self.bid_levels[pos];
            level.orders.retain(|&oid| oid != id);
            level.total_quantity = level.total_quantity.saturating_sub(quantity);

            if level.total_quantity == 0 {
                self.bid_levels.remove(pos);
                // Update best bid
                self.best_bid = self.bid_levels.first().map(|l| l.price);
            }
        }
    }

    fn remove_from_asks(&mut self, id: OrderId, price: Price, quantity: Quantity) {
        if let Some(pos) = self.ask_levels.iter().position(|l| l.price == price) {
            let level = &mut self.ask_levels[pos];
            level.orders.retain(|&oid| oid != id);
            level.total_quantity = level.total_quantity.saturating_sub(quantity);

            if level.total_quantity == 0 {
                self.ask_levels.remove(pos);
                // Update best ask
                self.best_ask = self.ask_levels.first().map(|l| l.price);
            }
        }
    }

    fn reduce_bid_quantity(&mut self, price: Price, quantity: Quantity) {
        if let Some(level) = self.bid_levels.iter_mut().find(|l| l.price == price) {
            level.total_quantity = level.total_quantity.saturating_sub(quantity);
        }
    }

    fn reduce_ask_quantity(&mut self, price: Price, quantity: Quantity) {
        if let Some(level) = self.ask_levels.iter_mut().find(|l| l.price == price) {
            level.total_quantity = level.total_quantity.saturating_sub(quantity);
        }
    }
}

impl Default for OrderBook {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_empty_orderbook() {
        let book = OrderBook::new();
        assert_eq!(book.best_bid(), INVALID_PRICE);
        assert_eq!(book.best_ask(), INVALID_PRICE);
        assert_eq!(book.bid_quantity_at(10000), 0);
        assert_eq!(book.ask_quantity_at(10000), 0);
    }

    #[test]
    fn test_add_buy_order() {
        let mut book = OrderBook::new();
        book.add_order(1, Side::Buy, 10000, 100);

        assert_eq!(book.best_bid(), 10000);
        assert_eq!(book.best_ask(), INVALID_PRICE);
        assert_eq!(book.bid_quantity_at(10000), 100);
    }

    #[test]
    fn test_add_sell_order() {
        let mut book = OrderBook::new();
        book.add_order(1, Side::Sell, 10100, 50);

        assert_eq!(book.best_bid(), INVALID_PRICE);
        assert_eq!(book.best_ask(), 10100);
        assert_eq!(book.ask_quantity_at(10100), 50);
    }

    #[test]
    fn test_multiple_orders_same_price() {
        let mut book = OrderBook::new();
        book.add_order(1, Side::Buy, 10000, 100);
        book.add_order(2, Side::Buy, 10000, 200);

        assert_eq!(book.best_bid(), 10000);
        assert_eq!(book.bid_quantity_at(10000), 300);
    }

    #[test]
    fn test_best_bid_is_highest() {
        let mut book = OrderBook::new();
        book.add_order(1, Side::Buy, 10000, 100);
        book.add_order(2, Side::Buy, 10100, 100);
        book.add_order(3, Side::Buy, 9900, 100);

        assert_eq!(book.best_bid(), 10100);
    }

    #[test]
    fn test_best_ask_is_lowest() {
        let mut book = OrderBook::new();
        book.add_order(1, Side::Sell, 10200, 100);
        book.add_order(2, Side::Sell, 10100, 100);
        book.add_order(3, Side::Sell, 10300, 100);

        assert_eq!(book.best_ask(), 10100);
    }

    #[test]
    fn test_cancel_order() {
        let mut book = OrderBook::new();
        book.add_order(1, Side::Buy, 10000, 100);
        book.add_order(2, Side::Buy, 10000, 200);

        assert!(book.cancel_order(1));
        assert_eq!(book.bid_quantity_at(10000), 200);
    }

    #[test]
    fn test_cancel_removes_price_level() {
        let mut book = OrderBook::new();
        book.add_order(1, Side::Buy, 10000, 100);
        book.cancel_order(1);

        assert_eq!(book.best_bid(), INVALID_PRICE);
        assert_eq!(book.bid_quantity_at(10000), 0);
    }

    #[test]
    fn test_partial_execution() {
        let mut book = OrderBook::new();
        book.add_order(1, Side::Buy, 10000, 100);
        book.execute_order(1, 30);

        assert_eq!(book.bid_quantity_at(10000), 70);
    }

    #[test]
    fn test_full_execution() {
        let mut book = OrderBook::new();
        book.add_order(1, Side::Buy, 10000, 100);
        book.execute_order(1, 100);

        assert_eq!(book.best_bid(), INVALID_PRICE);
        assert_eq!(book.bid_quantity_at(10000), 0);
    }

    #[test]
    fn test_cancel_nonexistent() {
        let mut book = OrderBook::new();
        assert!(!book.cancel_order(999));
    }
}
