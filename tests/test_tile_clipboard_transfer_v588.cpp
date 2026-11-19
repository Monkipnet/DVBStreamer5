#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/http/TileClipboardTransfer.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace dvbstreamer5::http::tile_clipboard;
int main() {
    Json::Value raw(Json::objectValue);
    raw["id"]="stream-existing";
    raw["name"]="Sport HD";
    raw["auto_start"]=true;
    raw["input_uri"]="https://user:password@server.example/live.ts?token=secret123#frag";
    raw["backup_input_uri"]="srt://host:5000?streamid=secret123";
    raw["srt_input_passphrase"]="secret123";
    raw["srt_output_streamid"]="secret123";
    raw["hls_access_key_value"]="secret123";
    raw["hls_encryption"]="aes-128";
    raw["hls_encryption_key_hex"]="secret123";
    raw["conditional_access_client"]="secret123";
    raw["interface_address"]="192.168.1.20";
    raw["stream_auth_mode"]="off";
    raw["xmltv_source_id"]="private-provider";
    Json::Value extra(Json::objectValue);
    extra["output_type"]="srt";
    extra["srt_passphrase"]="secret123";
    raw["additional_outputs"].append(extra);
    auto output=envelope("stream",raw);
    assert(isEnvelope(output));
    const auto tile=output["tile"];
    assert(tile["id"].asString().empty());
    assert(tile["auto_start"].asBool()==false);
    assert(tile["input_uri"].asString()=="https://server.example/live.ts");
    assert(tile["backup_input_uri"].asString()=="srt://host:5000");
    assert(tile["hls_encryption"].asString()=="none");
    assert(tile["stream_auth_mode"].asString()=="inherit");
    assert(tile["additional_outputs"][0]["srt_passphrase"].asString().empty());
    Json::StreamWriterBuilder writer;
    const auto txt=Json::writeString(writer,output);
    assert(txt.find("secret123")==std::string::npos);
    assert(txt.find("user:password")==std::string::npos);
    assert(txt.find("192.168.1.20")==std::string::npos);
    raw["input_uri"]="dvb://1#pnr=23";
    output=envelope("stream",raw);
    assert(output["tile"]["input_uri"].asString()=="dvb://1#pnr=23");
    auto wisi=envelope("wisi_epg",raw);
    assert(isEnvelope(wisi));
    assert(wisi["tile"]["enabled"].asBool()==false);
    assert(wisi["tile"]["auto_start"].asBool()==false);
    assert(!isEnvelope(Json::Value(Json::objectValue)));
    std::cout<<"PASS V588 tile clipboard: no secrets, WISI stopped, DVB URI intact\n";
}
