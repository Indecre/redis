#include "keyforge/common/version.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("the core library exposes a version") {
    REQUIRE_FALSE(keyforge::version().empty());
}

