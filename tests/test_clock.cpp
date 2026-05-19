#include <chrono>
#include <cmath>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "clock.h"

using Catch::Matchers::WithinAbs;

// =========================================================================
//  Constructor
// =========================================================================
TEST_CASE("Clock default construction", "[clock]")
{
    Clock c;

    CHECK(std::isnan(c.pts()));
    CHECK(c.serial() == -1);
    CHECK(std::isnan(c.get(c.serial())));
    CHECK(c.speed() == 1.0);
    CHECK_FALSE(c.paused());
}

// =========================================================================
//  set_at / get_at — deterministic, explicit virtual time
// =========================================================================
TEST_CASE("Clock set_at / get_at", "[clock]")
{
    SECTION("get_at same instant returns exact pts")
    {
        Clock c;
        c.set_at(42.5, 7, 0.0);
        REQUIRE_THAT(c.get_at(7, 0.0), WithinAbs(42.5, 0.0));
    }

    SECTION("set_at stores metadata fields")
    {
        Clock c;
        c.set_at(42.5, 7, 123.0);
        CHECK_THAT(c.pts(),          WithinAbs(42.5, 0.0));
        CHECK(c.serial() == 7);
        CHECK_THAT(c.last_updated(), WithinAbs(123.0, 0.0));
    }

    SECTION("wrong serial returns NaN")
    {
        Clock c;
        c.set_at(10.0, 5, 0.0);
        REQUIRE(std::isnan(c.get_at(99, 0.0)));
    }

    SECTION("get_at later advances at speed 1")
    {
        Clock c;
        c.set_at(10.0, 5, 0.0);
        REQUIRE_THAT(c.get_at(5, 3.0), WithinAbs(13.0, 1e-12));
    }

    SECTION("set_at overwrite resets baseline")
    {
        Clock c;
        c.set_at(10.0, 0, 0.0);
        c.set_at(30.0, 0, 8.0);
        REQUIRE_THAT(c.get_at(0, 10.0), WithinAbs(32.0, 1e-12));
    }
}


// =========================================================================
//  Paused
// =========================================================================
TEST_CASE("Clock pause / resume", "[clock]")
{
    SECTION("paused freezes value regardless of time")
    {
        Clock c;
        c.set_at(10.0, 0, 0.0);
        c.set_paused(true);
        REQUIRE_THAT(c.get_at(0, 999.0), WithinAbs(10.0, 0.0));
    }

    SECTION("unpause resumes advancing from frozen point")
    {
        Clock c;
        c.set_at(10.0, 0, 0.0);
        c.set_paused(true);
        c.set_paused(false);
        REQUIRE_THAT(c.get_at(0, 3.0), WithinAbs(13.0, 0.0));
    }

    SECTION("seek while paused then unpause")
    {
        Clock c;
        c.set_at(10.0, 0, 0.0);
        c.set_paused(true);
        c.set_at(50.0, 0, 5.0);   // seek while paused
        c.set_paused(false);
        REQUIRE_THAT(c.get_at(0, 7.0), WithinAbs(52.0, 0.0));
    }
}


// =========================================================================
//  Speed / drift — set_speed uses real clock internally;
//     we immediately re-anchor with set_at for deterministic checks.
// =========================================================================
TEST_CASE("Clock drift at non-default speed", "[clock]")
{
    SECTION("2× speed")
    {
        Clock c;
        c.set_speed(2.0);
        c.set_at(100.0, 0, 200.0);  // re-anchor at known virtual time
        REQUIRE_THAT(c.get_at(0, 210.0), WithinAbs(120.0, 1e-12));
    }

    SECTION("0.5× speed")
    {
        Clock c;
        c.set_speed(0.5);
        c.set_at(0.0, 0, 1000.0);
        REQUIRE_THAT(c.get_at(0, 1010.0), WithinAbs(5.0, 1e-12));
    }

    SECTION("0× speed — clock stops")
    {
        Clock c;
        c.set_speed(0.0);
        c.set_at(10.0, 0, 200.0);
        REQUIRE_THAT(c.get_at(0, 9999.0), WithinAbs(10.0, 1e-12));
    }

    SECTION("negative pts advances correctly")
    {
        Clock c;
        c.set_at(-5.0, 0, 0.0);
        REQUIRE_THAT(c.get_at(0, 3.0), WithinAbs(-2.0, 0.0));
    }
}

// =========================================================================
//  Formula brute-force
//
//     get_at(ser, t) = pts + (t - last_updated_) * speed
//
//     Cartesian product: 3 × 2 × 5 × 3 = 90 permutations.
//     Catch2 + GENERATE makes the runner log the exact failing
//     parameter set, which a raw for-loop cannot do cleanly.
// =========================================================================
TEST_CASE("Clock formula", "[clock]")
{
    auto pts = GENERATE(-10.0, 0.0, 10.0);
    auto t0  = GENERATE(1.0, 10.0);
    auto spd = GENERATE(0.0, 0.5, 1.0, 2.0, 4.0);
    auto dt  = GENERATE(0.0, 1.5, 7.0);

    Clock c;
    c.set_speed(spd);            // anchors to real time internally
    c.set_at(pts, 0, t0);        // override with exact virtual baseline

    double expected = pts + dt * spd;
    double actual   = c.get_at(0, t0 + dt);

    REQUIRE_THAT(actual, WithinAbs(expected, 1e-12));
}

// =========================================================================
//  Live: set() / get() — exercises av_gettime_relative() path
// =========================================================================

TEST_CASE("Clock set / get (live)", "[clock][live]")
{
    SECTION("set then immediate get")
    {
        Clock c;
        c.set(100.0, 1);
        REQUIRE_THAT(c.get(1), WithinAbs(100.0, 0.001));
    }

    SECTION("sleep 100 ms")
    {
        Clock c;
        c.set(0.0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        REQUIRE_THAT(c.get(0), WithinAbs(0.10, 0.05));
    }

    SECTION("paused ignores sleep")
    {
        Clock c;
        c.set(7.0, 0);
        c.set_paused(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        REQUIRE_THAT(c.get(0), WithinAbs(7.0, 0.001));
    }

    SECTION("set_speed preserves current value")
    {
        Clock c;
        c.set(50.0, 3);
        double before = c.get(3);
        c.set_speed(2.0);
        double after = c.get(3);
        REQUIRE_THAT(before, WithinAbs(after, 0.001));
    }
}

// =========================================================================
//  sync_clock_to_slave — core A/V drift correction
// =========================================================================
TEST_CASE("sync_clock_to_slave", "[clock][live]")
{
    SECTION("diverged beyond threshold — master pulled to slave")
    {
        Clock master, slave;
        master.set(100.0, 1);
        slave.set(200.0, 2);      // |diff| ≈ 100 > AV_NOSYNC_THRESHOLD (10.0)

        sync_clock_to_slave(&master, 1, &slave, 2);

        REQUIRE(master.serial() == 2);
        REQUIRE_THAT(master.get(2), WithinAbs(200.0, 0.001));
    }

    SECTION("within threshold — master unchanged")
    {
        Clock master, slave;
        master.set(100.0, 1);
        slave.set(103.0, 2);      // |diff| ≈ 3 < threshold

        sync_clock_to_slave(&master, 1, &slave, 2);

        REQUIRE(master.serial() == 1);            // unchanged
        REQUIRE_THAT(master.get(1), WithinAbs(100.0, 1.0));
    }

    SECTION("slave is NaN — master unchanged")
    {
        Clock master, slave;      // slave has NaN pts
        master.set(50.0, 1);

        sync_clock_to_slave(&master, 1, &slave, slave.serial());

        REQUIRE_THAT(master.get(1), WithinAbs(50.0, 0.001));
    }

    SECTION("master is NaN — master gets set")
    {
        Clock master;             // NaN
        Clock slave;
        slave.set(75.0, 2);

        sync_clock_to_slave(&master, master.serial(), &slave, 2);

        REQUIRE_THAT(master.get(master.serial()), WithinAbs(75.0, 0.001));
    }

    SECTION("stale slave serial — ignored because get returns NaN")
    {
        Clock master, slave;
        master.set(100.0, 1);
        slave.set(200.0, 2);

        sync_clock_to_slave(&master, 1, &slave, 999);

        REQUIRE_THAT(master.get(1), WithinAbs(100.0, 0.001));
    }
}
