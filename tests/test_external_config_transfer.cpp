#include "../src/http/ExternalConfigTransfer.h"
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
    assert(safeId("Channel #1")=="Channel__1");
    std::cout<<"PASS Cesbo Astra/Flussonic converters and safe append-only semantics\n";
}
