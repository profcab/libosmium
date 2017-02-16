#include "catch.hpp"

#include <osmium/builder/attr.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/tags/matcher.hpp>

TEST_CASE("Tag matcher") {
    osmium::memory::Buffer buffer{10240};

    const auto pos = osmium::builder::add_tag_list(buffer,
        osmium::builder::attr::_tags({
            { "highway", "primary" },
            { "name", "Main Street" },
            { "source", "GPS" }
    }));
    const osmium::TagList& tag_list = buffer.get<osmium::TagList>(pos);

    SECTION("Matching keys only") {
        osmium::TagMatcher m{osmium::StringMatcher::equal{"highway"}};
        REQUIRE(m(tag_list));

        REQUIRE(m(*tag_list.begin()));
        REQUIRE_FALSE(m(*std::next(tag_list.begin())));
    }

    SECTION("Matching keys only with shortcut const char*") {
        osmium::TagMatcher m{"highway"};
        REQUIRE(m(tag_list));

        REQUIRE(m(*tag_list.begin()));
        REQUIRE_FALSE(m(*std::next(tag_list.begin())));
    }

    SECTION("Matching keys only with shortcut std::string") {
        std::string s{"highway"};
        osmium::TagMatcher m{s};
        REQUIRE(m(tag_list));

        REQUIRE(m(*tag_list.begin()));
        REQUIRE_FALSE(m(*std::next(tag_list.begin())));
    }

    SECTION("Matching key and value") {
        osmium::TagMatcher m{osmium::StringMatcher::equal{"highway"},
                             osmium::StringMatcher::equal{"primary"}};
        REQUIRE(m(tag_list));

        REQUIRE(m(*tag_list.begin()));
        REQUIRE_FALSE(m(*std::next(tag_list.begin())));
    }

    SECTION("Matching key and value with shortcut") {
        osmium::TagMatcher m{"highway", "primary", false};
        REQUIRE(m(tag_list));

        REQUIRE(m(*tag_list.begin()));
        REQUIRE_FALSE(m(*std::next(tag_list.begin())));
    }

    SECTION("Matching key and value") {
        osmium::TagMatcher m{osmium::StringMatcher::equal{"highway"},
                             osmium::StringMatcher::equal{"secondary"}};
        REQUIRE_FALSE(m(tag_list));
    }

    SECTION("Matching key and value inverted") {
        osmium::TagMatcher m{osmium::StringMatcher::equal{"highway"},
                             osmium::StringMatcher::equal{"secondary"},
                             true};
        REQUIRE(m(tag_list));

        REQUIRE(m(*tag_list.begin()));
        REQUIRE_FALSE(m(*std::next(tag_list.begin())));
    }

    SECTION("Matching key and value list") {
        osmium::TagMatcher m{osmium::StringMatcher::equal{"highway"},
                             osmium::StringMatcher::list{{"primary", "secondary"}}};
        REQUIRE(m(tag_list));

        REQUIRE(m(*tag_list.begin()));
        REQUIRE_FALSE(m(*std::next(tag_list.begin())));
    }
}

