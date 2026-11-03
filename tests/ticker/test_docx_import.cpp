#include "http/HttpServerTickerImport.inc"
#include <zlib.h>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

static void expect(bool value, const char* what) {
    if(!value){std::cerr<<"DOCX import test failed: "<<what<<"\n";std::exit(1);}
}
static void le16(std::string& s, std::uint16_t n) {
    s.push_back(static_cast<char>(n&255));
    s.push_back(static_cast<char>((n>>8)&255));
}
static void le32(std::string& s, std::uint32_t n) {
    le16(s,static_cast<std::uint16_t>(n&65535));
    le16(s,static_cast<std::uint16_t>(n>>16));
}
static std::string makeDocx(const std::string& xml, bool compress) {
    const std::string name="word/document.xml";
    std::string bytes=xml;
    if(compress) {
        z_stream z{};
        expect(deflateInit2(&z,Z_DEFAULT_COMPRESSION,Z_DEFLATED,
                            -MAX_WBITS,8,Z_DEFAULT_STRATEGY)==Z_OK,"deflate init");
        bytes.resize(compressBound(xml.size()));
        z.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(xml.data()));
        z.avail_in=static_cast<uInt>(xml.size());
        z.next_out=reinterpret_cast<Bytef*>(&bytes[0]);
        z.avail_out=static_cast<uInt>(bytes.size());
        expect(deflate(&z,Z_FINISH)==Z_STREAM_END,"deflate");
        bytes.resize(z.total_out);
        deflateEnd(&z);
    }
    const std::uint32_t checksum=crc32(0,reinterpret_cast<const Bytef*>(xml.data()),
                                       static_cast<uInt>(xml.size()));
    std::string zip;
    le32(zip,0x04034b50);
    le16(zip,20);le16(zip,0);le16(zip,compress?8:0);
    le16(zip,0);le16(zip,0);
    le32(zip,checksum);le32(zip,bytes.size());le32(zip,xml.size());
    le16(zip,name.size());le16(zip,0);
    zip+=name;zip+=bytes;
    const auto centralOffset=static_cast<std::uint32_t>(zip.size());
    le32(zip,0x02014b50);
    le16(zip,20);le16(zip,20);le16(zip,0);le16(zip,compress?8:0);
    le16(zip,0);le16(zip,0);
    le32(zip,checksum);le32(zip,bytes.size());le32(zip,xml.size());
    le16(zip,name.size());le16(zip,0);le16(zip,0);
    le16(zip,0);le16(zip,0);le32(zip,0);le32(zip,0);
    zip+=name;
    const auto centralSize=static_cast<std::uint32_t>(zip.size())-centralOffset;
    le32(zip,0x06054b50);
    le16(zip,0);le16(zip,0);le16(zip,1);le16(zip,1);
    le32(zip,centralSize);le32(zip,centralOffset);le16(zip,0);
    return zip;
}
int main() {
    const std::string xml=
        R"doc(<?xml version="1.0" encoding="UTF-8"?>)"
        R"doc(<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">)"
        R"doc(<w:body><w:p><w:r><w:t>Привет &amp; мир</w:t></w:r></w:p>)"
        R"doc(<w:p><w:r><w:t>Вторая строка</w:t></w:r></w:p></w:body></w:document>)doc";
    for(const bool compressed:{false,true}) {
        std::string output,error;
        const auto zip=makeDocx(xml,compressed);
        expect(tickerImportDocx(zip,output,error),"extract DOCX");
        expect(output=="Привет & мир\nВторая строка","paragraphs and Cyrillic");
        std::string corrupted=zip;
        corrupted[50]^=1; // corrupt compressed data or payload
        expect(!tickerImportDocx(corrupted,output,error),"CRC or damaged ZIP");
    }
    std::string output,error;
    expect(!tickerImportDocx("not a zip",output,error),"reject non-ZIP input");
    expect(!tickerImportDocx(std::string(2U*1024U*1024U+1U,'x'),output,error),
           "reject oversized ZIP");
    std::cout<<"DOCX import tests OK\n";
    return 0;
}
