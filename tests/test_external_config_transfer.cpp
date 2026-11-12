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
          "udp://239.11.12.13:5000#ttl=8",
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
    assert(astra44Lua.find("secret")==std::string::npos);
    Conversion loopback;
    assert(astra44::importLegacy(astra44Lua,loopback,err));
    assert(loopback.streams.size()==2);
    assert(loopback.streams[0].inputServiceId==142);
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
