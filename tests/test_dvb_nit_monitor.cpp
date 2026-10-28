// Standalone NIT reader regression test. Does not require DVB hardware.
// Build: c++ -std=c++17 -Isrc tests/test_dvb_nit_monitor.cpp -o /tmp/test_dvb_nit_monitor
#include "media/DvbNitMonitor.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {
using Byte = std::uint8_t;
using Bytes = std::vector<Byte>;

std::uint32_t mpegCrc(const Bytes& section) {
    std::uint32_t crc = 0xffffffffU;
    for (const Byte value : section) {
        crc ^= static_cast<std::uint32_t>(value) << 24U;
        for (int i = 0; i < 8; ++i) {
            crc = crc & 0x80000000U ? (crc << 1U) ^ 0x04c11db7U
                                    : (crc << 1U);
        }
    }
    return crc;
}

Bytes makeNit(const std::string& name, bool large) {
    Bytes desc;
    if (large) {
        // Make the network descriptors span a TS packet boundary.
        desc.push_back(0x7fU);
        desc.push_back(180U);
        desc.insert(desc.end(), 180, 0x30U);
    }
    desc.push_back(0x40U); // network_name_descriptor
    desc.push_back(static_cast<Byte>(name.size()));
    desc.insert(desc.end(),name.begin(),name.end());

    Bytes section = {
        0x40U,0xb0U,0x00U, // NIT actual; section_length filled below
        0x00U,0x01U,       // network_id
        0xc1U,0x00U,0x00U, // current version; section_number; last_section_number
        static_cast<Byte>(0xf0U | ((desc.size() >> 8U) & 0x0fU)),
        static_cast<Byte>(desc.size() & 0xffU)
    };
    section.insert(section.end(),desc.begin(),desc.end());

    // One transport stream with satellite_delivery_system_descriptor 0x43.
    // Frequency BCD = 01238300 * 10 kHz = 12383000 kHz
    // Orbital position BCD = 0192 = 19.2 degrees E, horizontal polarity.
    const Bytes ts = {
        0x00U,0x01U,0x00U,0x01U, // TSID / ONID
        0xf0U,0x0dU,             // descriptors_loop_length = 13
        0x43U,0x0bU,
        0x01U,0x23U,0x83U,0x00U,
        0x01U,0x92U,
        0x80U,
        0x02U,0x75U,0x00U,0x00U
    };
    section.push_back(0xf0U);
    section.push_back(static_cast<Byte>(ts.size()));
    section.insert(section.end(),ts.begin(),ts.end());
    const std::size_t length = section.size()-3+4;
    section[1] = static_cast<Byte>(0xb0U | ((length>>8U) & 0x0fU));
    section[2] = static_cast<Byte>(length & 0xffU);
    const auto crc=mpegCrc(section);
    section.push_back(static_cast<Byte>((crc>>24U)&0xffU));
    section.push_back(static_cast<Byte>((crc>>16U)&0xffU));
    section.push_back(static_cast<Byte>((crc>>8U)&0xffU));
    section.push_back(static_cast<Byte>(crc&0xffU));
    return section;
}

Bytes packetize(const Bytes& section) {
    Bytes bytes;
    std::size_t consumed=0;
    unsigned cc=0;
    while(consumed<section.size()){
        Bytes pkt(188,0xffU);
        const bool start=consumed==0;
        pkt[0]=0x47U;
        pkt[1]=start?0x40U:0x00U;
        pkt[2]=0x10U; // PID 0x0010
        pkt[3]=static_cast<Byte>(0x10U | (cc++&0x0fU));
        const auto pos=start?5U:4U;
        if(start)pkt[4]=0U; // pointer_field
        const auto count=std::min<std::size_t>(188-pos,section.size()-consumed);
        std::copy_n(section.data()+consumed,count,pkt.data()+pos);
        consumed+=count;
        bytes.insert(bytes.end(),pkt.begin(),pkt.end());
    }
    return bytes;
}
}

int main() {
    using dvbstreamer5::media::network::DvbNitMonitor;

    // A short, valid NIT section with correct CRC.
    DvbNitMonitor one(12383000U,"H");
    const Bytes valid=packetize(makeNit("Astra Test",false));
    one.observe(valid.data(),valid.size());
    assert(one.snapshot().networkName=="Astra Test");
    assert(one.snapshot().orbitalPosition=="19.2°E");

    // An unrelated RF must not give the frontend the wrong orbital position.
    DvbNitMonitor other(11605000U,"H");
    other.observe(valid.data(),valid.size());
    assert(other.snapshot().networkName=="Astra Test");
    assert(other.snapshot().orbitalPosition.empty());

    // Invalid CRC and NIT-other (0x41) cannot overwrite metadata.
    DvbNitMonitor bad(12383000U,"H");
    Bytes broken=valid;
    broken[8]^=1U;
    bad.observe(broken.data(),broken.size());
    assert(bad.snapshot().networkName.empty());
    Bytes nitOther=makeNit("Other Network",false);
    nitOther[0]=0x41U;
    const auto crc=mpegCrc(Bytes(nitOther.begin(),nitOther.end()-4));
    nitOther[nitOther.size()-4]=static_cast<Byte>(crc>>24U);
    nitOther[nitOther.size()-3]=static_cast<Byte>(crc>>16U);
    nitOther[nitOther.size()-2]=static_cast<Byte>(crc>>8U);
    nitOther[nitOther.size()-1]=static_cast<Byte>(crc);
    Bytes otherPackets=packetize(nitOther);
    bad.observe(otherPackets.data(),otherPackets.size());
    assert(bad.snapshot().networkName.empty());

    // Multi-packet section, no value should appear until section is complete.
    DvbNitMonitor fragmented(12383000U,"H");
    const Bytes multiple=packetize(makeNit("Astra Multi",true));
    assert(multiple.size()>=376);
    fragmented.observe(multiple.data(),188);
    assert(fragmented.snapshot().networkName.empty());
    fragmented.observe(multiple.data()+188,multiple.size()-188);
    assert(fragmented.snapshot().networkName=="Astra Multi");
    assert(fragmented.snapshot().orbitalPosition=="19.2°E");

    std::cout << "DVB NIT smoke tests passed\n";
}
