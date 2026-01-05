//! FFI (Foreign Function Interface) bindings for the HFT library
//!
//! This module provides C-compatible functions for interoperability
//! with C/C++ code.

use crate::orderbook::OrderBook;
use crate::types::*;
use std::os::raw::c_char;

/// Opaque OrderBook handle for C FFI
pub struct HftOrderBook {
    inner: OrderBook,
}

/// Side enum for C FFI (matches C API)
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HftSide {
    Buy = 0,
    Sell = 1,
}

impl From<HftSide> for Side {
    fn from(s: HftSide) -> Self {
        match s {
            HftSide::Buy => Side::Buy,
            HftSide::Sell => Side::Sell,
        }
    }
}

impl From<Side> for HftSide {
    fn from(s: Side) -> Self {
        match s {
            Side::Buy => HftSide::Buy,
            Side::Sell => HftSide::Sell,
        }
    }
}

/// Quote structure for C FFI
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct HftQuote {
    pub bid_price: Price,
    pub ask_price: Price,
    pub bid_size: Quantity,
    pub ask_size: Quantity,
}

/// Trade structure for C FFI
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct HftTrade {
    pub aggressive_order_id: OrderId,
    pub passive_order_id: OrderId,
    pub price: Price,
    pub quantity: Quantity,
    pub aggressor_side: HftSide,
    pub timestamp: u64,
}

// ============================================
// OrderBook FFI Functions
// ============================================

/// Create a new order book
///
/// # Safety
/// Returns a raw pointer that must be freed with `hft_orderbook_destroy`
#[no_mangle]
pub extern "C" fn hft_rust_orderbook_create() -> *mut HftOrderBook {
    let book = Box::new(HftOrderBook {
        inner: OrderBook::new(),
    });
    Box::into_raw(book)
}

/// Destroy an order book
///
/// # Safety
/// The pointer must be valid and must have been created by `hft_rust_orderbook_create`
#[no_mangle]
pub unsafe extern "C" fn hft_rust_orderbook_destroy(book: *mut HftOrderBook) {
    if !book.is_null() {
        drop(Box::from_raw(book));
    }
}

/// Add an order to the book
///
/// # Safety
/// The book pointer must be valid
#[no_mangle]
pub unsafe extern "C" fn hft_rust_orderbook_add_order(
    book: *mut HftOrderBook,
    order_id: OrderId,
    side: HftSide,
    price: Price,
    quantity: Quantity,
) -> bool {
    if book.is_null() {
        return false;
    }

    let book = &mut *book;
    book.inner.add_order(order_id, side.into(), price, quantity);
    true
}

/// Cancel an order
///
/// # Safety
/// The book pointer must be valid
#[no_mangle]
pub unsafe extern "C" fn hft_rust_orderbook_cancel_order(
    book: *mut HftOrderBook,
    order_id: OrderId,
) -> bool {
    if book.is_null() {
        return false;
    }

    let book = &mut *book;
    book.inner.cancel_order(order_id)
}

/// Execute (partial fill) an order
///
/// # Safety
/// The book pointer must be valid
#[no_mangle]
pub unsafe extern "C" fn hft_rust_orderbook_execute_order(
    book: *mut HftOrderBook,
    order_id: OrderId,
    quantity: Quantity,
) -> bool {
    if book.is_null() {
        return false;
    }

    let book = &mut *book;
    book.inner.execute_order(order_id, quantity)
}

/// Get best bid price
///
/// # Safety
/// The book pointer must be valid
#[no_mangle]
pub unsafe extern "C" fn hft_rust_orderbook_best_bid(book: *const HftOrderBook) -> Price {
    if book.is_null() {
        return INVALID_PRICE;
    }

    let book = &*book;
    book.inner.best_bid()
}

/// Get best ask price
///
/// # Safety
/// The book pointer must be valid
#[no_mangle]
pub unsafe extern "C" fn hft_rust_orderbook_best_ask(book: *const HftOrderBook) -> Price {
    if book.is_null() {
        return INVALID_PRICE;
    }

    let book = &*book;
    book.inner.best_ask()
}

/// Get quantity at bid price level
///
/// # Safety
/// The book pointer must be valid
#[no_mangle]
pub unsafe extern "C" fn hft_rust_orderbook_bid_quantity_at(
    book: *const HftOrderBook,
    price: Price,
) -> Quantity {
    if book.is_null() {
        return 0;
    }

    let book = &*book;
    book.inner.bid_quantity_at(price)
}

/// Get quantity at ask price level
///
/// # Safety
/// The book pointer must be valid
#[no_mangle]
pub unsafe extern "C" fn hft_rust_orderbook_ask_quantity_at(
    book: *const HftOrderBook,
    price: Price,
) -> Quantity {
    if book.is_null() {
        return 0;
    }

    let book = &*book;
    book.inner.ask_quantity_at(price)
}

// ============================================
// Utility Functions
// ============================================

/// Convert price from double to fixed-point
#[no_mangle]
pub extern "C" fn hft_rust_price_from_double(price: f64) -> Price {
    (price * 10000.0 + 0.5) as Price
}

/// Convert price from fixed-point to double
#[no_mangle]
pub extern "C" fn hft_rust_price_to_double(price: Price) -> f64 {
    price as f64 / 10000.0
}

/// Get library version
#[no_mangle]
pub extern "C" fn hft_rust_version() -> *const c_char {
    b"0.1.0\0".as_ptr() as *const c_char
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_ffi_orderbook_lifecycle() {
        unsafe {
            let book = hft_rust_orderbook_create();
            assert!(!book.is_null());

            // Add order
            let result = hft_rust_orderbook_add_order(book, 1, HftSide::Buy, 10000, 100);
            assert!(result);

            // Check best bid
            let bid = hft_rust_orderbook_best_bid(book);
            assert_eq!(bid, 10000);

            // Cancel order
            let cancelled = hft_rust_orderbook_cancel_order(book, 1);
            assert!(cancelled);

            // Destroy
            hft_rust_orderbook_destroy(book);
        }
    }

    #[test]
    fn test_price_conversion() {
        let price = hft_rust_price_from_double(150.25);
        assert_eq!(price, 1502500);

        let back = hft_rust_price_to_double(1502500);
        assert!((back - 150.25).abs() < 0.0001);
    }
}
