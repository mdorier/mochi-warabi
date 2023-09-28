/*
 * (C) 2020 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */
#include <warabi/Admin.hpp>
#include <warabi/Provider.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_all.hpp>

static constexpr const char* target_config = "{ \"path\" : \"mydb\" }";

TEST_CASE("Admin tests", "[admin]") {

    auto target_type = GENERATE(as<std::string>{}, "memory");

    auto engine = thallium::engine("na+sm", THALLIUM_SERVER_MODE);
    // Initialize the provider
    warabi::Provider provider(engine);

    SECTION("Create an admin") {
        warabi::Admin admin(engine);
        std::string addr = engine.self();

        SECTION("Create and destroy targets") {
            /* correct target creation */
            warabi::UUID target_id = admin.addTarget(addr, 0, target_type, target_config);

            /* target creation with a bad target type */
            REQUIRE_THROWS_AS(admin.addTarget(addr, 0, "blabla", target_config),
                              warabi::Exception);

            /* target creation with a bad configuration */
            REQUIRE_THROWS_AS(admin.addTarget(addr, 0, target_type, "{["),
                              warabi::Exception);

            /* correctly destroy target */
            admin.destroyTarget(addr, 0, target_id);

            /* destroy a target with an ID that does not exist */
            warabi::UUID bad_id;
            REQUIRE_THROWS_AS(admin.destroyTarget(addr, 0, bad_id), warabi::Exception);
        }
    }
    // Finalize the engine
    engine.finalize();
}
