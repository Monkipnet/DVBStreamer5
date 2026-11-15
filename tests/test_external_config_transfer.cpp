#include "../src/http/ExternalConfigTransfer.h"
#include "../src/http/AstraLegacy44.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <string>

using namespace dvbstreamer5::external_config;

int main() {
    Conversion out;
    std::string err;
    // Official Cesbo make_stream SPTS schema. Do not import the unhandled MPTS.
    const std::string astra=R"({
      "make_stream":[
        {"id":"a001","name":"Astra Test","type":"spts","enable":true,
         "input":["udp://239.1.2.3:1234","tshttp://origin.local:8080/tv"],
         "output":["udp://239.3.2.1:5000","rtp://239.3.2.2:5001"]},
        {"id":"a002","name":"MPTS Test","type":"mpts",
         "input":["udp://239.0.0.1:1234"]}
      ],
      "users":{"admin":{"password":"SECRET_DO_NOT_IMPORT"}}
    })";
    assert(parseAstra(astra,out,err));
    assert(out.streams.size()==1 && out.ignored==1);
    assert(out.streams[0].name=="Astra Test");
    assert(out.streams[0].inputUri=="udp://239.1.2.3:1234");
    assert(out.streams[0].backupInputUri=="http://origin.local:8080/tv");
    assert(out.streams[0].outputType=="udp-vbr");
    assert(out.streams[0].outputHost=="239.3.2.1");
    assert(out.streams[0].additionalOutputs.size()==1);
    assert(!out.streams[0].autoStart);
    assert(out.streams[0].conditionalAccessClient.empty());
    std::vector<StreamConfig> existing;
    auto preset=baseStream("production");
    preset.id="production";preset.inputUri="udp://239.42.0.1:5000";
    existing.push_back(preset);
    size_t duplicates=0;
    assert(mergeNewOnly(out,existing,"astra",duplicates)==1);
    assert(existing.size()==2 && existing[0].id=="production");
    assert(!existing[1].autoStart);
    assert(mergeNewOnly(out,existing,"astra",duplicates)==0 && duplicates==1);

    std::vector<std::string> warnings;
    const auto exported=exportAstra(existing,warnings);
    assert(exported["make_stream"].isArray() &&
           exported["make_stream"].size()==2);
    assert(exported["make_stream"][1]["input"].size()==2);
    assert(exported["make_stream"][1]["enable"].asBool()==false);

    // V532: representative Astra JSON fixtures derived from real 4.x
    // satellite and terrestrial exports. Deliberately no CAM credentials,
    // subscriber data, tokens or private backend values in this test.
    const std::string astraDvbJson=R"ASTRA532({
      "dvb_tune":[
        {"id":"a001","type":"S2","adapter":6,"device":0,
         "frequency":"11785.02","symbolrate":"27500","polarization":"V",
         "lof1":"9750","lof2":"10600","slof":"10750","diseqc":"1",
         "uni_frequency":""},
        {"id":"a073","type":"S","adapter":0,"device":0,
         "frequency":"11823","symbolrate":"27500","polarization":"H"},
        {"id":"a0be","type":"T2","adapter":8,"device":0,
         "frequency":"626","bandwidth":"8MHz"},
        {"id":"a0c5","type":"T2","adapter":10,"device":0,
         "frequency":"205","bandwidth":"7MHz"}
      ],
      "make_stream":[
        {"id":"a005","name":"Satellite Astra","type":"spts","enable":true,
         "input":["dvb://a001#pnr=1302&cam=a0b1"],
         "output":["http://192.0.2.1:5555/matchHD"]},
        {"id":"a0co","name":"Terrestrial T2","type":"spts","enable":true,
         "input":["dvb://a0be#pnr=12"],"output":["http://192.0.2.1:5553/PADA"]},
        {"id":"a0es","name":"T2 7MHz","type":"spts",
         "input":["dvb://a0c5#pnr=2"],"output":["udp://239.23.2.5:1234"]},
        {"id":"t2mi","name":"Unsupported encapsulation","type":"spts",
         "input":["t2mi://a001#pnr=100"],"output":["http://192.0.2.1:5555/t2mi"]}
      ],
      "users":{"admin":{"password":"SECRET_DO_NOT_IMPORT"}},
      "softcam":[{"id":"a0b1","type":"newcamd","pass":"SECRET_DO_NOT_IMPORT"}]
    })ASTRA532";
    Conversion astraDvb;
    assert(parseAstra(astraDvbJson,astraDvb,err));
    assert(astraDvb.streams.size()==3 && astraDvb.ignored==1);
    const auto& sat=astraDvb.streams[0];
    assert(sat.name=="Satellite Astra");
    assert(sat.inputUri.find("dvb://satellite?adapter=6&frontend=0") == 0);
    assert(sat.inputUri.find("frequency_khz=11785020")!=std::string::npos);
    assert(sat.inputUri.find("symbol_rate=27500")!=std::string::npos);
    assert(sat.inputUri.find("lnb_lof1_khz=9750000")!=std::string::npos);
    assert(sat.inputUri.find("lnb_slof_khz=10750000")!=std::string::npos);
    assert(sat.inputServiceId==1302U);
    assert(sat.tvgId=="a005");
    assert(sat.outputType=="http");
    assert(sat.outputMode=="listener");
    assert(sat.outputPort==0); // existing HTTP server port
    assert(!sat.autoStart && sat.conditionalAccessClient.empty());
    assert(sat.toJson()["tvg_id"].asString()=="a005");
    assert(StreamConfig::fromJson(sat.toJson()).tvgId=="a005");
    assert(astraDvb.streams[1].tvgId=="a0co");
    assert(astraDvb.streams[1].inputUri.find("dvb://terrestrial?adapter=8&frontend=0") == 0);
    assert(astraDvb.streams[1].inputUri.find("frequency_khz=626000") != std::string::npos);
    assert(astraDvb.streams[1].inputUri.find("bandwidth_hz=8000000") != std::string::npos);
    assert(astraDvb.streams[1].inputServiceId==12U);
    assert(astraDvb.streams[1].outputType=="http");
    assert(astraDvb.streams[2].inputUri.find("frequency_khz=205000")!=std::string::npos);
    assert(astraDvb.streams[2].inputUri.find("bandwidth_hz=7000000")!=std::string::npos);
    assert(astraDvb.streams[2].outputType=="udp-vbr");
    assert(!astraDvb.streams[2].autoStart);
    bool hasT2miWarning=false,hasCaWarning=false,hasHttpWarning=false;
    for(const auto& w:astraDvb.warnings){
        hasT2miWarning |= w.find("T2MI input skipped")!=std::string::npos;
        hasCaWarning |= w.find("CA/CAM references not transferred")!=std::string::npos;
        hasHttpWarning |= w.find("Astra HTTP output mapped")!=std::string::npos;
        assert(w.find("SECRET_DO_NOT_IMPORT")==std::string::npos);
    }
    assert(hasT2miWarning&&hasCaWarning&&hasHttpWarning);
    std::vector<StreamConfig> merged;
    size_t dvbDuplicates=0;
    assert(mergeNewOnly(astraDvb,merged,"astra",dvbDuplicates)==3);
    assert(merged.front().id.find("migration_astra_")==0);
    assert(merged.front().id!="a005" && merged.front().tvgId=="a005");
    assert(mergeNewOnly(astraDvb,merged,"astra",dvbDuplicates)==0);
    assert(dvbDuplicates==3);

    // Astra CP1251 exports contain raw non-UTF8 channel titles.
    const std::string cp1251=
        std::string(R"({"make_stream":[{"id":"a12","type":"spts","name":")")+
        "\"" + std::string({char(0xCC),char(0xE8),char(0xF0)}) +
        R"(","input":["udp://239.1.1.1:1234"]}]})";
    Conversion cp1251Out;
    assert(parseAstra(cp1251,cp1251Out,err));
    assert(cp1251Out.streams.size()==1);
    assert(cp1251Out.streams[0].name==std::string("\xD0\x9C\xD0\xB8\xD1\x80"));
    assert(!cp1251Out.warnings.empty() &&
           cp1251Out.warnings[0].find("Windows-1251")!=std::string::npos);
    assert(cp1251Out.streams[0].tvgId=="a12");
    assert(!parseAstra(R"({"dvb_tune":[{"id":"broken","type":"S2",
      "adapter":0,"frequency":"11823","symbolrate":"bad"}],
      "make_stream":[{"name":"no-frequency","input":["dvb://broken#pnr=30"]}]})",
      cp1251Out,err));
    std::cout<<"PASS Astra JSON satellite/T2 tuners, HTTP MPEGTS, tvg-id, CP1251 and safe T2MI refusal\\n";

    const std::string flussonic=
        "http 8080;\n"
        "stream news_one {\n"
        " input udp://239.5.5.5:1234;\n"
        " input tshttp://origin.local/live;\n"
        " push udp://239.6.6.6:6000;\n"
        " dvr /var/lib/flussonic;\n"
        "}\n"
        "stream music_two { input rtp://239.1.1.1:5050; }\n";
    assert(parseFlussonic(flussonic,out,err));
    assert(out.streams.size()==2);
    assert(out.streams[0].inputUri=="udp://239.5.5.5:1234");
    assert(out.streams[0].backupInputUri=="http://origin.local/live");
    assert(out.streams[0].outputPort==6000);
    assert(out.streams[1].inputUri=="rtp://239.1.1.1:5050");
    warnings.clear();
    const auto conf=exportFlussonic(out.streams,warnings);
    assert(conf.find("stream news_one {")!=std::string::npos);
    assert(conf.find("push udp://239.6.6.6:6000;")!=std::string::npos);
    assert(conf.find("SECRET_DO_NOT_IMPORT")==std::string::npos);

    assert(!parseAstra(R"({"make_stream":[{"name":"x","type":"mpts"}]})",out,err));
    assert(!parseFlussonic("stream broken { input udp://239.1.1.1:1234;",out,err));
    assert(safeId("Channel #1")=="Channel_1");

    // Cesbo Astra 4.4.182 used Lua make_channel(), not JSON make_stream.
    // A Lua interpreter MUST NEVER be invoked on imported configuration.
    const std::string legacyAstra = R"ASTRA44(
      -- make_channel({ name="fake", input={"udp://239.99.1.1:5000"} })
      adapter_sat = dvb_tune({
        adapter="1", device=0, type="S2",
        tp="11836:H:27500", lnb="9750:10600:11700", diseqc=2,
      })
      reader0 = newcamd({host="127.0.0.1",user="sensitive",pass="secret"})
      make_channel({
        name = "Legacy Discovery",
        enable = true,
        service_provider = "Test Broadcaster",
        input = {
          "dvb://adapter_sat#pnr=142&cam=reader0",
          "http://backup.example.net/live.ts",
        },
        output = {
          "udp://192.168.1.5@239.11.12.13:5000#ttl=8",
          "rtp://239.11.12.14:5002",
        },
      })
      make_channel({
        name = "Old IP",
        input = {"udp://239.2.2.2:1234"},
        output = {"udp://239.3.3.3:7000"},
        enable = false,
      })
    )ASTRA44";
    Conversion legacy;
    assert(astra44::importLegacy(legacyAstra,legacy,err));
    assert(legacy.streams.size()==2);
    assert(legacy.streams[0].inputUri.find("dvb://satellite?adapter=1&frontend=0") == 0);
    assert(legacy.streams[0].inputUri.find("frequency_khz=11836000") != std::string::npos);
    assert(legacy.streams[0].inputUri.find("lnb_lof1_khz=9750000") != std::string::npos);
    assert(legacy.streams[0].inputServiceId==142);
    assert(legacy.streams[0].backupInputUri=="http://backup.example.net/live.ts");
    assert(legacy.streams[0].serviceProvider=="Test Broadcaster");
    assert(legacy.streams[0].outputType=="udp-vbr");
    assert(legacy.streams[0].outputHost=="239.11.12.13");
    assert(legacy.streams[0].interfaceAddress=="192.168.1.5");
    assert(legacy.streams[0].outputPort==5000);
    assert(legacy.streams[0].additionalOutputs.size()==1);
    assert(!legacy.streams[0].autoStart);
    assert(legacy.streams[0].conditionalAccessClient.empty());
    assert(legacy.streams[1].inputUri=="udp://239.2.2.2:1234");
    assert(legacy.streams[1].outputPort==7000);
    assert(legacy.warnings.size()>=1); // CAM/TTL are not silently migrated

    warnings.clear();
    const auto astra44Lua=astra44::exportLegacy(legacy.streams,warnings);
    assert(astra44Lua.find("dvb_tune({")!=std::string::npos);
    assert(astra44Lua.find("make_channel({")!=std::string::npos);
    assert(astra44Lua.find("dvb5_tune_1")!=std::string::npos);
    assert(astra44Lua.find("udp://192.168.1.5@239.11.12.13:5000")!=std::string::npos);
    assert(astra44Lua.find("secret")==std::string::npos);
    Conversion loopback;
    assert(astra44::importLegacy(astra44Lua,loopback,err));
    assert(loopback.streams.size()==2);
    assert(loopback.streams[0].inputServiceId==142);
    assert(loopback.streams[0].interfaceAddress=="192.168.1.5");
    assert(loopback.streams[0].backupInputUri=="http://backup.example.net/live.ts");
    assert(loopback.streams[1].inputUri=="udp://239.2.2.2:1234");

    // Dynamic Lua expressions are never evaluated and never accepted.
    const auto dangerous=R"ASTRA44(
      make_channel({
        name="unsafe",
        input={os.execute("touch /tmp/DVBStreamer5_ASTRA_LUA_EXECUTED")},
        output={"udp://239.1.2.3:4000"},
      })
    )ASTRA44";
    assert(!astra44::importLegacy(dangerous,loopback,err));
    assert(!astra44::importLegacy(R"({"make_stream":[]})",loopback,err));
    // Mixed Lua literal list vs object is rejected; no best-effort eval.
    assert(!astra44::importLegacy(
        "make_channel({ name='bad', input={'udp://239.1.1.1:5000', foo=1} })",
        loopback,err));
    assert(astra44::luaQuote("safe\"name\n").find("\\\"") != std::string::npos);
    std::cout<<"PASS Cesbo Astra 4.4.18x Lua literal migration and roundtrip\n";
    std::cout<<"PASS Cesbo Astra/Flussonic converters and safe append-only semantics\n";
}
