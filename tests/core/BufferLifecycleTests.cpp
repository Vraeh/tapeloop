// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/BufferLifecycle.hpp"

#include <catch2/catch_test_macros.hpp>

using tapeloop::BufferLifecycle;

TEST_CASE("buffers start and stop with the outputs when set to")
{
	BufferLifecycle lifecycle;
	CHECK(lifecycle.startWithOutputs());
	CHECK_FALSE(lifecycle.running());

	lifecycle.setStreaming(true);
	CHECK(lifecycle.running());
	lifecycle.setRecording(true);
	lifecycle.setStreaming(false);
	CHECK(lifecycle.running());
	lifecycle.setRecording(false);
	CHECK_FALSE(lifecycle.running());

	lifecycle.setRecording(true);
	CHECK(lifecycle.running());
	lifecycle.setRecording(false);
	CHECK_FALSE(lifecycle.running());
}

TEST_CASE("the manual control is refused while an output runs with auto start on")
{
	BufferLifecycle lifecycle;
	CHECK(lifecycle.manualControlEnabled());
	lifecycle.setStreaming(true);
	CHECK_FALSE(lifecycle.manualControlEnabled());
	CHECK_FALSE(lifecycle.manualStop());
	CHECK(lifecycle.running());
	CHECK_FALSE(lifecycle.manualStart());

	lifecycle.setStreaming(false);
	lifecycle.setRecording(true);
	CHECK_FALSE(lifecycle.manualControlEnabled());
	CHECK_FALSE(lifecycle.manualStop());
	CHECK(lifecycle.running());
}

TEST_CASE("the manual control works between outputs with auto start on")
{
	BufferLifecycle lifecycle;
	CHECK(lifecycle.manualStart());
	CHECK(lifecycle.running());
	CHECK(lifecycle.manualStart());
	CHECK(lifecycle.manualStop());
	CHECK_FALSE(lifecycle.running());
	CHECK(lifecycle.manualStop());

	// Started by hand, the buffers still stop once both outputs have stopped.
	CHECK(lifecycle.manualStart());
	lifecycle.setStreaming(true);
	lifecycle.setStreaming(false);
	CHECK_FALSE(lifecycle.running());
}

TEST_CASE("without auto start only the manual control moves the buffers, at any time")
{
	BufferLifecycle lifecycle(false);
	lifecycle.setStreaming(true);
	lifecycle.setRecording(true);
	CHECK_FALSE(lifecycle.running());
	CHECK(lifecycle.manualControlEnabled());
	CHECK(lifecycle.manualStart());
	CHECK(lifecycle.running());
	lifecycle.setStreaming(false);
	lifecycle.setRecording(false);
	CHECK(lifecycle.running());
	lifecycle.setStreaming(true);
	CHECK(lifecycle.manualStop());
	CHECK_FALSE(lifecycle.running());
}

TEST_CASE("starting over follows the new setting and the outputs")
{
	BufferLifecycle lifecycle;
	lifecycle.setStreaming(true);
	REQUIRE(lifecycle.running());

	lifecycle.reset(false, true, false);
	CHECK_FALSE(lifecycle.running());
	CHECK(lifecycle.streaming());
	CHECK(lifecycle.manualControlEnabled());
	CHECK(lifecycle.manualStart());

	lifecycle.reset(true, false, true);
	CHECK(lifecycle.running());
	CHECK(lifecycle.recording());
	CHECK_FALSE(lifecycle.streaming());

	lifecycle.reset(true, false, false);
	CHECK_FALSE(lifecycle.running());
}

TEST_CASE("changing the auto start setting")
{
	BufferLifecycle lifecycle(false);
	lifecycle.setStreaming(true);
	lifecycle.setStartWithOutputs(true);
	CHECK(lifecycle.running());
	CHECK_FALSE(lifecycle.manualControlEnabled());

	lifecycle.setStartWithOutputs(false);
	CHECK(lifecycle.running());
	CHECK(lifecycle.manualControlEnabled());
	lifecycle.setStreaming(false);
	CHECK(lifecycle.running());

	// Turning it on with no output active leaves buffers started by hand running.
	lifecycle.setStartWithOutputs(true);
	CHECK(lifecycle.running());
	CHECK(lifecycle.manualStop());
	CHECK_FALSE(lifecycle.running());
}
