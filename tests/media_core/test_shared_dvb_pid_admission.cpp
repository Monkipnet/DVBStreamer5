#include "media/SharedDvbPidAdmission.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

using dvbstreamer5::media::network::SharedDvbPidAdmission;
using dvbstreamer5::media::network::filterSharedDvbPidChunk;

static void appendPacket(std::vector<std::uint8_t>& out, std::uint16_t pid) {
    const std::size_t start = out.size();
    out.resize(start + 188, 0xff);
    out[start] = 0x47;
    out[start + 1] = static_cast<std::uint8_t>((pid >> 8) & 0x1f);
    out[start + 2] = static_cast<std::uint8_t>(pid & 0xff);
    out[start + 3] = 0x10;
}

static std::uint16_t packetPid(
    const std::shared_ptr<const std::vector<std::uint8_t>>& data,
    std::size_t packet) {
    const auto base = packet * 188;
    return static_cast<std::uint16_t>(
        ((data->at(base + 1) & 0x1fU) << 8) | data->at(base + 2));
}

int main() {
    auto mutableInput = std::make_shared<std::vector<std::uint8_t>>();
    for (auto pid: {0U, 1U, 0x12U, 100U, 101U, 102U, 0x1fffU})
        appendPacket(*mutableInput, static_cast<std::uint16_t>(pid));
    std::shared_ptr<const std::vector<std::uint8_t>> input = mutableInput;
    auto state = std::make_shared<SharedDvbPidAdmission>();

    // No PMT yet: never discard packets before an admission map exists.
    assert(filterSharedDvbPidChunk(input, state) == input);
    auto mask = std::make_shared<SharedDvbPidAdmission::Mask>();
    mask->fill(0);
    for (auto pid: {0U, 1U, 100U, 101U}) (*mask)[pid] = 1;
    state->publish(mask);
    auto result = filterSharedDvbPidChunk(input, state);
    assert(result && result->size() == 5 * 188);
    for (std::size_t i=0; i<5; ++i) {
        const std::uint16_t expected[] = {0, 1, 0x12, 100, 101};
        assert(packetPid(result, i) == expected[i]);
    }
    // EIT is retained for EPG even if normal service remap omits its PID.
    assert(packetPid(result, 2) == 0x12);
    // New PMT generation switches the selected media PIDs atomically.
    auto next = std::make_shared<SharedDvbPidAdmission::Mask>();
    next->fill(0);
    (*next)[0] = (*next)[1] = (*next)[100] = (*next)[102] = 1;
    state->publish(next);
    result = filterSharedDvbPidChunk(input, state);
    assert(result && result->size() == 5 * 188);
    assert(packetPid(result, 4) == 102);
    // Damaged alignment must never cause silent source-level drops.
    auto misaligned = std::make_shared<std::vector<std::uint8_t>>(*input);
    misaligned->push_back(0x00);
    assert(filterSharedDvbPidChunk(misaligned, state) == misaligned);
    std::cout << "Shared DVB per-service PID admission: OK\n";
}
