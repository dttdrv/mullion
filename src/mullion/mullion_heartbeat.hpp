#pragma once
#include <cstdint>
#include <functional>

namespace mullion::heartbeat {

// macOS unwires a process's GPU memory a while after its GPU goes idle, and the next submission waits for all of it
// to be wired again: on an M6 (macOS 27.3) a frame after 10 s with 8 GiB resident took 135 ms to complete instead of
// 3 (tools/metal-bench/residency_idle.swift; llama.cpp found the same, ggml-org/llama.cpp#17766). A game that pauses,
// loads or loses focus stutters on its first frames after. While no queue has submitted work for a second, each
// watched set is asked to stay resident (MTLResidencySet's requestResidency) about once a second, for up to three
// minutes, after which macOS may take the memory of a game left in the background. MULLION_RESIDENCY_HEARTBEAT=0
// turns this off.

// keep_resident commits its residency set and requests its residency, under the lock its owner changes it with;
// it runs on the heartbeat's thread. The id is for Forget
uint64_t Watch(std::function<void()> keep_resident);

// no more calls of the set's keep_resident after this returns
void Forget(uint64_t id);

// a queue submitted work: the GPU is busy, and the heartbeat waits for the next idle second
void Submitted();

} // namespace mullion::heartbeat
