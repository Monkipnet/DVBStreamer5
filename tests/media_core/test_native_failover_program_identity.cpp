#include "media/NativeFailoverProgramIdentity.h"
#include "media/NativeMpegTsMux.h"
#include "media/MpegTsRemapper.h"
#include "media/TransportStream.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dvbstreamer5::media::mpegts;
static void check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

static std::vector<Packet> muxProgram(std::uint16_t sid,
        std::uint16_t pmtPid, std::uint16_t videoPid,
        std::uint16_t audioPid, const std::string& name, bool hasAudio=true) {
    NativeMpegTsMux mux;
    NativeMuxConfig cfg;
    cfg.serviceId=sid;cfg.pmtPid=pmtPid;
    cfg.videoPid=videoPid;cfg.audioPid=audioPid;cfg.serviceName=name;
    std::string error;
    check(mux.initialize(cfg,error),"init TS mux failed");
    check(mux.setCodec(ElementaryKind::Video,ElementaryCodec::H264,error),
        "H264 mux failed");
    if (hasAudio)
        check(mux.setCodec(ElementaryKind::Audio,ElementaryCodec::AacAdts,error),
            "AAC mux failed");
    const std::uint8_t video[]={0,0,0,1,0x65,0x88,0x44};
    const std::uint8_t audio[]={0xff,0xf1,0x50,0x80,0x01,0x1f,0xfc};
    std::vector<Packet> packets;
    ElementarySample v;
    v.data=video;v.size=sizeof(video);
    v.pts90k=90000;v.dts90k=90000;
    v.hasPts=true;v.hasDts=true;v.randomAccess=true;
    check(mux.write(ElementaryKind::Video,v,packets,error),"write video failed");
    ElementarySample a;
    a.data=audio;a.size=sizeof(audio);
    a.pts90k=90000;a.dts90k=90000;
    a.hasPts=true;a.hasDts=true;
    if (hasAudio)
        check(mux.write(ElementaryKind::Audio,a,packets,error),"write audio failed");
    return packets;
}

static std::uint16_t pidOf(const Packet& packet) {
    return static_cast<std::uint16_t>(((packet[1] & 0x1fU)<<8U)|packet[2]);
}

int main() {
    try {
        constexpr std::uint16_t sid=313, pmt=0x1200, video=0x0220, audio=0x0221;
        const auto original=muxProgram(sid,pmt,video,audio,
            "\xD0\xA8\xD0\xB0\xD0\xBD\xD1\x81\xD0\xBE\xD0\xBD\xD0\xA2\xD0\x92");
        NativeFailoverProgramIdentity identity;
        identity.observe(original);
        const auto primary=identity.snapshot();
        check(primary.serviceId==sid,"primary SID not discovered from PAT");
        check(primary.pmtPid==pmt,"primary PMT PID not discovered from PAT");
        check(primary.videoPid==video,"primary video PID not discovered from PMT");
        check(primary.audioPid==audio,"primary audio PID not discovered from PMT");

        const auto rawBackup=muxProgram(7,0x1000,0x0100,0x0101,"backup1");
        RemapConfig cfg;
        cfg.inputServiceId=0;cfg.inputAudioPid=0;
        cfg.outputServiceId=primary.serviceId;
        cfg.outputPmtPid=primary.pmtPid;
        cfg.outputVideoPid=primary.videoPid;
        cfg.outputAudioPid=primary.audioPid;
        cfg.serviceName="\xD0\xA8\xD0\xB0\xD0\xBD\xD1\x81\xD0\xBE\xD0\xBD\xD0\xA2\xD0\x92";
        cfg.serviceProvider="DVBStreamer5";
        Remapper remap;
        std::string error;
        check(remap.initialize(cfg,error),"backup remapper failed");
        std::vector<Packet> output;
        for(const auto& p:rawBackup)
            check(remap.process(p,output,error),"failed to remap backup media");
        check(!output.empty(),"no remapped backup packets");
        bool havePat=false,havePmt=false,haveVideo=false,haveAudio=false,haveSdt=false;
        for(const auto& p:output) {
            switch(pidOf(p)) {
                case 0: havePat=true;break;
                case pmt: havePmt=true;break;
                case video: haveVideo=true;break;
                case audio: haveAudio=true;break;
                case 0x11: haveSdt=true;break;
                default: break;
            }
        }
        check(havePat&&havePmt&&haveVideo&&haveAudio&&haveSdt,
            "backup service metadata/payload PIDs not preserved");
        NativeFailoverProgramIdentity observedBackup;
        observedBackup.observe(output);
        const auto mapped=observedBackup.snapshot();
        check(mapped.serviceId==sid&&mapped.pmtPid==pmt&&
              mapped.videoPid==video&&mapped.audioPid==audio,
              "remapped backup identity differs from primary");
        // backup1 name must disappear from SDT; Cyrillic name remains encoded.
        for(const auto& p:output)if(pidOf(p)==0x11U){
            const std::string bytes(reinterpret_cast<const char*>(p.data()),p.size());
            check(bytes.find("backup1")==std::string::npos,
                  "backup1 leaked through SDT");
        }
        // The configured primary contains video and audio. A video-only MP4
        // replacement must NOT terminate the stream because audio is absent.
        const auto videoOnly=muxProgram(9,0x1000,0x100,0x101,"backup1",false);
        RemapConfig strictConfig=cfg;
        Remapper strict;
        check(strict.initialize(strictConfig,error),"strict remap init");
        bool strictFailed=false;
        for(const auto& packet:videoOnly){
            std::vector<Packet> ignored;
            if(!strict.process(packet,ignored,error)){strictFailed=true;break;}
        }
        check(strictFailed,"ordinary remaps must still reject missing tracks");
        RemapConfig backupConfig=cfg;
        backupConfig.allowMissingOutputMedia=true;
        Remapper backupRemap;
        check(backupRemap.initialize(backupConfig,error),"backup remap init");
        std::vector<Packet> preserved;
        for(const auto& packet:videoOnly)
            check(backupRemap.process(packet,preserved,error),
                "video-only backup must not kill remapper");
        NativeFailoverProgramIdentity videoOnlyObserved;
        videoOnlyObserved.observe(preserved);
        const auto videoOnlyProgram=videoOnlyObserved.snapshot();
        check(videoOnlyProgram.serviceId==sid&&videoOnlyProgram.pmtPid==pmt&&
              videoOnlyProgram.videoPid==video&&videoOnlyProgram.audioPid==0,
              "video-only backup must preserve SID/PMT/video and omit audio");
        // Simulate the third tile: its SRT input is the second tile's
        // video-only backup, yet its saved input audio PID came from the
        // original primary. Once primary returns, PMT must change version.
        RemapConfig downstream{};
        downstream.inputServiceId = sid;
        downstream.outputServiceId = 800;
        downstream.outputPmtPid = 0x1300;
        downstream.outputVideoPid = 0x0400;
        downstream.outputAudioPid = 0x0401;
        downstream.inputAudioPid = audio;
        downstream.allowMissingOutputMedia = true;
        downstream.serviceName = "shanson3";
        Remapper downstreamRemap;
        check(downstreamRemap.initialize(downstream,error),
              "downstream SRT remapper init failed");
        std::vector<Packet> downstreamBackup;
        for(const auto& packet:preserved)
            check(downstreamRemap.process(packet,downstreamBackup,error),
                  "SRT downstream video-only PMT must not kill channel");
        NativeFailoverProgramIdentity downstreamIdentity;
        downstreamIdentity.observe(downstreamBackup);
        const auto downstreamSaved = downstreamIdentity.snapshot();
        check(downstreamSaved.serviceId == 800 &&
              downstreamSaved.pmtPid == 0x1300 &&
              downstreamSaved.videoPid == 0x0400 &&
              downstreamSaved.audioPid == 0,
              "downstream SRT remap did not retain video-only service");
        auto currentPmtVersion=[](const std::vector<Packet>& pkts) -> int {
            for(const auto& packet:pkts) {
                if(pidOf(packet)==0x1300 && (packet[1]&0x40U) &&
                   packet[4]==0U && packet[5]==0x02U)
                    return (packet[10]>>1U)&0x1fU;
            }
            return -1;
        };
        const int backupVersion=currentPmtVersion(downstreamBackup);
        check(backupVersion>=0, "missing downstream backup PMT");
        std::vector<Packet> downstreamRestored;
        for(const auto& packet:original)
            check(downstreamRemap.process(packet,downstreamRestored,error),
                  "SRT downstream cannot accept restored primary");
        // pmt version is emitted during the next TS packet after PMT arrival.
        std::vector<Packet> nextPsi;
        downstreamRemap.tick(nextPsi);
        downstreamRestored.insert(downstreamRestored.end(),nextPsi.begin(),nextPsi.end());
        const int restoredVersion=currentPmtVersion(downstreamRestored);
        check(restoredVersion>=0 && restoredVersion!=backupVersion,
              "downstream PMT version not advanced after audio restored");
        // The downstream video decoder also needs an elementary PID
        // discontinuity, not only a PMT version change. Assert the marker
        // comes before the first restored video payload and is TS-valid.
        bool sawVideoMarker=false;
        bool firstVideoPayloadAfterMarker=false;
        for (const auto& ts : downstreamRestored) {
            if (pidOf(ts) != 0x0400) continue;
            PacketInfo info{};
            check(inspectPacket(ts.data(),ts.size(),info),
                  "invalid video TS packet after downstream failback");
            if (!info.hasPayload && info.discontinuity) {
                sawVideoMarker = true;
                continue;
            }
            if (info.hasPayload && sawVideoMarker) {
                firstVideoPayloadAfterMarker = true;
                break;
            }
        }
        check(sawVideoMarker && firstVideoPayloadAfterMarker,
              "missing video discontinuity before restored primary PES");
        NativeFailoverProgramIdentity restoredIdentity;
        restoredIdentity.observe(downstreamRestored);
        const auto restored=restoredIdentity.snapshot();
        check(restored.serviceId==800 && restored.pmtPid==0x1300 &&
              restored.videoPid==0x0400 && restored.audioPid==0x0401,
              "downstream remapper did not resume audio after primary return");
        std::cout<<"PASS: three-tile primary/backup SRT PMT transitions and recovery\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"FAIL: "<<e.what()<<"\n";
        return 1;
    }
}
