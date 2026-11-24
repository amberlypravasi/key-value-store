// Tests for the in-memory store (stage 1).

#include "doctest.h"
#include "kvstore/store.h"

using kvstore::Store;

TEST_CASE("a new store is empty") {
    Store store;
    CHECK(store.size() == 0);
    CHECK_FALSE(store.get("missing").has_value());
}

TEST_CASE("get returns what set stored") {
    Store store;
    store.set("name", "Alisha");
    CHECK(store.get("name") == "Alisha");
    CHECK(store.size() == 1);
}

TEST_CASE("set overwrites an existing key") {
    Store store;
    store.set("color", "blue");
    store.set("color", "green");
    CHECK(store.get("color") == "green");
    CHECK(store.size() == 1);
}

TEST_CASE("remove deletes a key and reports whether it existed") {
    Store store;
    store.set("temp", "value");
    CHECK(store.remove("temp"));
    CHECK_FALSE(store.get("temp").has_value());
    CHECK_FALSE(store.remove("temp"));
    CHECK(store.size() == 0);
}

TEST_CASE("empty strings are valid keys and values") {
    Store store;
    store.set("", "");
    CHECK(store.get("") == "");
}
