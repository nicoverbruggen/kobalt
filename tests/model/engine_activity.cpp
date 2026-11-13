#include "engine/crengine/engine_activity.h"
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <memory>

using namespace kobalt;
using namespace std::chrono_literals;

void check(bool value)
{
    if (!value)
        std::abort();
}

template <class Predicate>
void waitFor(Predicate predicate)
{
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        check(std::chrono::steady_clock::now() < end);
        std::this_thread::yield();
    }
}

int main()
{
    EngineActivity gate;
    std::atomic<int> stage{ 0 };
    gate.pause(true);
    std::thread worker([&] {
        stage = 1;
        gate.enter();
        stage = 2;
        waitFor([&] { return stage == 3; });
        gate.checkpoint();
        stage = 4;
        gate.leave();
    });
    waitFor([&] { return stage == 1 && gate.quiescent(); });
    std::this_thread::sleep_for(20ms);
    check(stage == 1); // A queued job cannot start during sleep.
    gate.pause(false);
    waitFor([&] { return stage == 2; });
    check(!gate.quiescent()); // Do not permit suspend while work is running.
    gate.pause(true);
    stage = 3;
    waitFor([&] { return gate.quiescent(); });
    std::this_thread::sleep_for(20ms);
    check(stage == 3); // A formatting checkpoint stops an existing job.
    gate.pause(false);
    worker.join();
    check(stage == 4 && gate.quiescent());
    // Repeated wake/cancel calls must leave the worker usable.
    gate.pause(false);
    gate.pause(true);
    gate.pause(false);
    gate.enter();
    check(!gate.quiescent());
    gate.leave();
    check(gate.quiescent());

    // A save spans worker jobs and GUI replies. Overlapping saves keep the
    // requested pause deferred until their final owner leaves.
    gate.pause(true);
    auto first = std::make_unique<EngineDrain>(gate);
    auto second = std::make_unique<EngineDrain>(gate);
    check(!gate.quiescent());
    gate.enter();
    gate.checkpoint();
    gate.leave();
    first.reset();
    check(!gate.quiescent());
    second.reset();
    check(gate.quiescent());
    stage = 0;
    std::thread paused([&] {
        stage = 1;
        gate.enter();
        stage = 2;
        gate.leave();
    });
    waitFor([&] { return stage == 1 && gate.quiescent(); });
    std::this_thread::sleep_for(20ms);
    check(stage == 1);
    // Starting a required save also releases a job already parked at sleep.
    {
        EngineDrain drain(gate);
        paused.join();
        check(stage == 2 && !gate.quiescent());
        // Wake while draining must not be undone when the drain ends.
        gate.pause(false);
    }
    gate.enter();
    gate.leave();
    check(gate.quiescent());
}
