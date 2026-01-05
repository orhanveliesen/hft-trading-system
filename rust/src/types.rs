/// Fixed-point price: 4 decimal places (e.g., 12345 = $1.2345)
pub type Price = u32;
pub type Quantity = u32;
pub type OrderId = u64;

pub const INVALID_PRICE: Price = u32::MAX;
pub const INVALID_ORDER_ID: OrderId = 0;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Side {
    Buy,
    Sell,
}

#[derive(Debug, Clone)]
pub struct Order {
    pub id: OrderId,
    pub price: Price,
    pub quantity: Quantity,
    pub side: Side,
}

impl Order {
    pub fn new(id: OrderId, side: Side, price: Price, quantity: Quantity) -> Self {
        Self { id, price, quantity, side }
    }
}
