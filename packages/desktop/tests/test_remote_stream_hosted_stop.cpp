// The owner's stop of a hosting stream that ended otherwise first (`SandboxRemoteHostEnd::stop`): the instant of that end it tells, in the suite after the hosting stream's other tests: `twidget-tests remote_stream_hosted_`.
#include "test_support.h"
#include "test_remote_support.h"
#include "test_remote_stream_support.h"

#include "twidget/json.h"
#include "twidget/remote_hosted.h"
#include "twidget/remote_stream.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

using Clock = std::chrono::steady_clock;

/// An accounting stream's reading, as an owner that keeps its renderers' answer times opens one, that keeps no message: what the stop tells depends on none.
static SandboxHostedAnswers::Reading keepsNothing(std::string_view) { return {}; }

/// An accounting stream's reading that times each host's message, one its renderer answers and never does here: the oldest one's answer time ends the stream.
static SandboxHostedAnswers::Reading expectsEach(std::string_view) { return {SandboxHostedAnswers::Role::Expects}; }

/// How a hosting stream with the owner's SandboxHostedAnswers ends when a renderer's answer time ran out, as `test_remote_stream_hosted.cpp` reads it.
static const std::string kUnanswered = "a hosted renderer left a platform message unanswered for more than 3000 ms";

/// `at` in ms after `from`, for a check's message: "none" without it.
static std::string msAfter(const std::optional<Clock::time_point>& at, Clock::time_point from) {
    return at ? Json::number(std::chrono::duration<double, std::milli>(*at - from).count()) + " ms" : std::string("none");
}

/// The owner's stop after the stream ended otherwise tells the instant of that end, taken as the end came: after the moment the end was made possible and before the stop was called, and a second stop the same one. After the engine process refused the stream before `Ready`, once `ended` reported it, a widget attached afterwards is `Spared`; after the engine process closed the stream while the reader's fan-out is held in a widget's loss, the stop tells it before `ended` reported that end. After the monitor's look found A's answer time run out, the stop tells the instant that look judged the answer times at, the one each widget's death was decided at: at or after A's message's admission plus its answer time, no later than `ended`, and on the side of B's answer time, a second younger, that B's death says (spared: before it ran out; stalled: at or after it). The owner's own stop, the stream's first end, tells none: `remote_stream_hosted_answers_decide_each_death_once_at_the_end`.
TEST(remote_stream_hosted_stop_tells_the_instant_of_an_end_before_it) {
    {
        ScriptedEngine engine({}, true, {keepsNothing});
        CHECK_MSG(eventually([&] { return engine.saw(0, "hosting-opening"); }), joined(engine.frames(0)));
        const Clock::time_point before = Clock::now();
        CHECK(engine.send(FrameBytes(0, wire::Refused).str("locked: no").bytes()));
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }) && endsOnce(engine.events().all(), "the engine process refused the stream: locked: no"),
                  joined(engine.events().all()));
        const Clock::time_point stopping = Clock::now();
        const std::optional<Clock::time_point> first = engine.host().stop(), second = engine.host().stop();
        CHECK_MSG(first && second == first && before <= *first && *first <= stopping,
                  "first " + msAfter(first, before) + ", second " + msAfter(second, before) + ", the stop at " + msAfter(stopping, before));
        // A late widget's death as the end decided it, read as the host's own stop reads it.
        const std::shared_ptr<SandboxHostedChannel> attached = engine.host().hostedChannel();
        attached->attach(std::make_shared<StreamSink>());
        CHECK(attached->localStop() == SandboxHostedChannel::Death::Spared);
    }
    {
        // The reader's fan-out waits on it: declared before the end that joins the reader.
        Gate lost;
        ScriptedEngine engine({}, true, {keepsNothing});
        CHECK(engine.ready());
        auto sink = std::make_shared<StreamSink>();
        sink->lost = &lost;
        const std::shared_ptr<SandboxHostedChannel> route = engine.host().hostedChannel();
        route->attach(sink);
        // Its `Open` written first: nothing the writer still has fails at the close.
        CHECK_MSG(eventually([&] { return engine.saw(1, "open"); }), joined(engine.frames(1)));
        const Clock::time_point before = Clock::now();
        engine.close();
        CHECK_MSG(lost.waitedOn() && !engine.events().ended(), joined(engine.events().all()));
        const Clock::time_point stopping = Clock::now();
        Background<std::optional<Clock::time_point>> stopped([&] { return engine.host().stop(); });
        // The stop joins the reader, still held in the fan-out, so the end is not reported meanwhile.
        CHECK_MSG(!stopped.within(0.3) && !engine.events().ended(), joined(engine.events().all()));
        lost.open();
        const std::optional<Clock::time_point> first = stopped.finish(5), second = engine.host().stop();
        CHECK_MSG(first && second == first && before <= *first && *first <= stopping,
                  "first " + msAfter(first, before) + ", second " + msAfter(second, before) + ", the stop at " + msAfter(stopping, before));
        CHECK_MSG(endsOnce(engine.events().all(), "the engine process closed the stream") && sink->saw("disconnected"), joined(engine.events().all()) + " / " + joined(sink->events()));
    }
    {
        using Death = SandboxHostedChannel::Death;
        Gate monitor;
        ScriptedEngine engine({}, true, {expectsEach});
        CHECK(engine.ready());
        const std::shared_ptr<SandboxHostedChannel> a = engine.host().hostedChannel(), b = engine.host().hostedChannel();
        struct OpenMonitorAtExit {
            Gate& gate;
            ~OpenMonitorAtExit() { gate.open(); }
        };
        const OpenMonitorAtExit release{monitor};
        engine.events().holdAt = 2;
        engine.events().hold = &monitor;
        a->attach(std::make_shared<StreamSink>());
        b->attach(std::make_shared<StreamSink>());
        CHECK(monitor.waitedOn());
        const Clock::duration answerTime = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(SandboxEngine::kPlatformAnswerSeconds));
        // Each message's time runs from its admission, inside its `platform` call; B's a second after A's.
        const Clock::time_point aBefore = Clock::now();
        CHECK(a->platform("ask:1"));
        std::this_thread::sleep_until(aBefore + std::chrono::seconds(1));
        const Clock::time_point bBefore = Clock::now();
        CHECK(b->platform("ask:1"));
        const Clock::time_point bAfter = Clock::now();
        monitor.open();
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }, 3 * SandboxEngine::kPlatformAnswerSeconds) && endsOnce(engine.events().all(), kUnanswered),
                  joined(engine.events().all()));
        const std::optional<Clock::time_point> observed = engine.events().endedAt();
        // Each widget's death as the end decided it, read as the host's own stop reads it.
        const std::optional<Death> aDeath = a->localStop(), bDeath = b->localStop();
        const std::optional<Clock::time_point> first = engine.host().stop(), second = engine.host().stop();
        const auto named = [](const std::optional<Death>& death) { return !death ? "none" : *death == Death::Stalled ? "stalled" : *death == Death::Spared ? "spared" : "ordinary"; };
        const bool bSpared = bDeath == Death::Spared;
        CHECK_MSG(aDeath == Death::Stalled && (bSpared || bDeath == Death::Stalled), std::string("A ") + named(aDeath) + ", B " + named(bDeath));
        // The instant the deaths were decided at: A's time had run out by it, and B's (admitted between `bBefore` and `bAfter`) exactly when B is stalled.
        CHECK_MSG(first && second == first && observed && aBefore + answerTime <= *first && *first <= *observed &&
                      (bSpared ? *first < bAfter + answerTime : bBefore + answerTime <= *first),
                  "first " + msAfter(first, aBefore) + ", second " + msAfter(second, aBefore) + ", ended " + msAfter(observed, aBefore) + ", A's time out from " +
                      msAfter(aBefore + answerTime, aBefore) + ", B's between " + msAfter(bBefore + answerTime, aBefore) + " and " + msAfter(bAfter + answerTime, aBefore) +
                      ", B " + named(bDeath));
        if (first && observed)
            printf("    an answer time's end told %.1f ms after A's could run out, %.1f ms before ended; B %s\n", std::chrono::duration<double, std::milli>(*first - (aBefore + answerTime)).count(),
                   std::chrono::duration<double, std::milli>(*observed - *first).count(), named(bDeath));
        fflush(stdout);
    }
}
