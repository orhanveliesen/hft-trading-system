#include "../include/book_side.hpp"

#include <cassert>
#include <iostream>

using namespace hft;

// Helper to create a PriceLevel
PriceLevel* create_level(Price price, Quantity qty = 100) {
    PriceLevel* level = new PriceLevel();
    level->price = price;
    level->total_quantity = qty;
    level->head = nullptr;
    level->next = nullptr;
    level->prev = nullptr;
    return level;
}

void test_insert_single_level() {
    BidSide bid(10000, 1000);

    PriceLevel* level = create_level(10050);
    bid.insert_level(level);

    assert(bid.best_price() == 10050);
    assert(bid.quantity_at(10050) == 100);

    delete level;
}

void test_insert_multiple_levels() {
    BidSide bid(10000, 1000);

    PriceLevel* level1 = create_level(10050);
    PriceLevel* level2 = create_level(10040);
    PriceLevel* level3 = create_level(10030);

    bid.insert_level(level1);
    bid.insert_level(level2);
    bid.insert_level(level3);

    // For bids, best price is highest
    assert(bid.best_price() == 10050);

    delete level1;
    delete level2;
    delete level3;
}

void test_remove_level_if_empty() {
    BidSide bid(10000, 1000);

    PriceLevel* level1 = create_level(10050);
    PriceLevel* level2 = create_level(10040);

    bid.insert_level(level1);
    bid.insert_level(level2);

    // Make level2 empty
    level2->total_quantity = 0;

    // Remove level2 (it has a prev pointer - level1)
    // This covers line 143: level->prev->next = level->next
    PriceLevel* removed = bid.remove_level_if_empty(level2);
    assert(removed == level2);

    // Best price should still be level1
    assert(bid.best_price() == 10050);
    assert(bid.quantity_at(10040) == 0);

    delete level1;
    delete level2;
}

void test_remove_best_level() {
    AskSide ask(10000, 1000);

    PriceLevel* level1 = create_level(10010);
    PriceLevel* level2 = create_level(10020);

    ask.insert_level(level1);
    ask.insert_level(level2);

    // For asks, best price is lowest
    assert(ask.best_price() == 10010);

    // Make level1 empty and remove it
    level1->total_quantity = 0;
    PriceLevel* removed = ask.remove_level_if_empty(level1);
    assert(removed == level1);

    // Best price should now be level2
    assert(ask.best_price() == 10020);

    delete level1;
    delete level2;
}

void test_for_each_level() {
    BidSide bid(10000, 1000);

    PriceLevel* level1 = create_level(10050, 100);
    PriceLevel* level2 = create_level(10040, 200);
    PriceLevel* level3 = create_level(10030, 300);

    bid.insert_level(level1);
    bid.insert_level(level2);
    bid.insert_level(level3);

    int count = 0;
    bid.for_each_level([&count](Price price, Quantity qty) {
        count++;
        // Bids are sorted from best (highest) to worst (lowest)
        if (count == 1)
            assert(price == 10050);
        if (count == 2)
            assert(price == 10040);
        if (count == 3)
            assert(price == 10030);
    });

    assert(count == 3);

    delete level1;
    delete level2;
    delete level3;
}

void test_out_of_range_price() {
    BidSide bid(10000, 100);

    // Price 10200 is out of range (base=10000, range=100)
    PriceLevel* level = create_level(10200);
    bid.insert_level(level);

    // Should not crash, but find_level should return nullptr
    assert(bid.find_level(10200) == nullptr);
    assert(bid.quantity_at(10200) == 0);

    delete level;
}

int main() {
    test_insert_single_level();
    test_insert_multiple_levels();
    test_remove_level_if_empty();
    test_remove_best_level();
    test_for_each_level();
    test_out_of_range_price();

    std::cout << "All book_side tests passed!\n";
    return 0;
}
