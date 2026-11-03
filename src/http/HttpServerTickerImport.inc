// Local DOCX -> UTF-8 text import for the authenticated ticker editor.
// Only word/document.xml is extracted from the ZIP container. No external
// services, executable converters, shell commands or file-system writes.
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <zlib.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace {
static std::uint16_t tickerZip16(const std::string& data, std::size_t offset) {
    return static_cast<std::uint16_t>(
        static_cast<unsigned char>(data[offset]) |
        (static_cast<unsigned char>(data[offset+1]) << 8));
}
static std::uint32_t tickerZip32(const std::string& data, std::size_t offset) {
    return static_cast<std::uint32_t>(tickerZip16(data, offset)) |
           (static_cast<std::uint32_t>(tickerZip16(data, offset+2)) << 16);
}
static bool tickerZipWithin(const std::string& data, std::size_t start, std::size_t length) {
    return start <= data.size() && length <= data.size()-start;
}

static bool tickerExtractWordXml(const std::string& zip, std::string& xml, std::string& error) {
    constexpr std::size_t kMaxDocxBytes = 2U * 1024U * 1024U;
    constexpr std::size_t kMaxXmlBytes = 8U * 1024U * 1024U;
    if (zip.size() < 22 || zip.size() > kMaxDocxBytes) {
        error="DOCX file must not exceed 2 MiB"; return false;
    }
    const std::size_t searchStart=zip.size()>65557?zip.size()-65557:0;
    std::size_t eocd=std::string::npos;
    for (std::size_t p=zip.size()-22;;--p) {
        if (tickerZipWithin(zip,p,22) && tickerZip32(zip,p)==0x06054b50U &&
            tickerZipWithin(zip,p,22U+tickerZip16(zip,p+20)) &&
            p+22U+tickerZip16(zip,p+20)==zip.size()) {
            eocd=p;break;
        }
        if (p==searchStart)break;
    }
    if (eocd==std::string::npos || tickerZip16(zip,eocd+4)!=0 ||
        tickerZip16(zip,eocd+6)!=0) {
        error="Invalid DOCX ZIP directory";return false;
    }
    const std::size_t count=tickerZip16(zip,eocd+10);
    const std::size_t centralSize=tickerZip32(zip,eocd+12);
    const std::size_t centralOffset=tickerZip32(zip,eocd+16);
    if (count>20000 || !tickerZipWithin(zip,centralOffset,centralSize)) {
        error="Invalid DOCX central directory bounds";return false;
    }
    std::size_t pos=centralOffset;
    for (std::size_t i=0;i<count;++i) {
        if(!tickerZipWithin(zip,pos,46) || tickerZip32(zip,pos)!=0x02014b50U) {
            error="Invalid DOCX ZIP entry";return false;
        }
        const std::uint16_t flags=tickerZip16(zip,pos+8);
        const std::uint16_t method=tickerZip16(zip,pos+10);
        const std::uint32_t expectedCrc=tickerZip32(zip,pos+16);
        const std::size_t compressed=tickerZip32(zip,pos+20);
        const std::size_t uncompressed=tickerZip32(zip,pos+24);
        const std::size_t nameLen=tickerZip16(zip,pos+28);
        const std::size_t extraLen=tickerZip16(zip,pos+30);
        const std::size_t commentLen=tickerZip16(zip,pos+32);
        const std::size_t localOffset=tickerZip32(zip,pos+42);
        const std::size_t total=46U+nameLen+extraLen+commentLen;
        if(!tickerZipWithin(zip,pos,total)) {
            error="Truncated DOCX ZIP entry";return false;
        }
        if(zip.compare(pos+46,nameLen,"word/document.xml")!=0) {
            pos+=total;continue;
        }
        if((flags&1U)!=0 || (method!=8 && method!=0) ||
            uncompressed==0 || uncompressed>kMaxXmlBytes ||
            compressed>kMaxDocxBytes || (method==0 && compressed!=uncompressed)) {
            error="Unsupported or oversized DOCX document.xml";return false;
        }
        if(!tickerZipWithin(zip,localOffset,30) || tickerZip32(zip,localOffset)!=0x04034b50U) {
            error="Invalid DOCX local header";return false;
        }
        const std::size_t dataOffset=localOffset+30U+
            tickerZip16(zip,localOffset+26)+tickerZip16(zip,localOffset+28);
        if(!tickerZipWithin(zip,dataOffset,compressed)) {
            error="Truncated DOCX compressed document";return false;
        }
        xml.assign(uncompressed,'\0');
        if(method==0) {
            std::memcpy(xml.data(),zip.data()+dataOffset,uncompressed);
        } else {
            z_stream zs{};
            zs.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(zip.data()+dataOffset));
            zs.avail_in=static_cast<uInt>(compressed);
            zs.next_out=reinterpret_cast<Bytef*>(xml.data());
            zs.avail_out=static_cast<uInt>(uncompressed);
            if(inflateInit2(&zs,-MAX_WBITS)!=Z_OK) {
                error="Cannot initialize DOCX decompressor";return false;
            }
            const int result=inflate(&zs,Z_FINISH);
            const bool valid=result==Z_STREAM_END && zs.total_out==uncompressed &&
                zs.total_in==compressed;
            inflateEnd(&zs);
            if(!valid) {error="DOCX decompression failed";return false;}
        }
        const auto crc=crc32(0,reinterpret_cast<const Bytef*>(xml.data()),
                             static_cast<uInt>(xml.size()));
        if(crc!=expectedCrc) {error="DOCX CRC mismatch";return false;}
        return true;
    }
    error="DOCX has no word/document.xml";return false;
}

static bool tickerXmlWordTag(const xmlNode* node, const char* name) {
    static constexpr const char* uri=
        "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
    return node && node->type==XML_ELEMENT_NODE && node->ns &&
        node->ns->href && xmlStrEqual(node->ns->href,BAD_CAST uri) &&
        xmlStrEqual(node->name,BAD_CAST name);
}

static bool tickerParseWordText(const std::string& xml, std::string& text, std::string& error) {
    xmlDocPtr doc=xmlReadMemory(xml.data(),static_cast<int>(xml.size()),
        "document.xml","UTF-8",XML_PARSE_NONET|XML_PARSE_NOERROR|XML_PARSE_NOWARNING);
    if(!doc) {error="Invalid DOCX XML";return false;}
    constexpr std::size_t kMaxText=128U*1024U;
    bool exceeded=false;
    std::function<void(xmlNode*)> visit=[&](xmlNode* node) {
        for(;node && !exceeded;node=node->next) {
            if(tickerXmlWordTag(node,"t")) {
                xmlChar* value=xmlNodeGetContent(node);
                if(value) {
                    const std::size_t length=xmlStrlen(value);
                    if(text.size()+length>kMaxText) exceeded=true;
                    else text.append(reinterpret_cast<const char*>(value),length);
                    xmlFree(value);
                }
            } else if(tickerXmlWordTag(node,"tab")) {
                text.push_back('\t');
            } else if(tickerXmlWordTag(node,"br") || tickerXmlWordTag(node,"cr")) {
                text.push_back('\n');
            } else {
                visit(node->children);
                if(tickerXmlWordTag(node,"p"))text.push_back('\n');
            }
            if(text.size()>kMaxText)exceeded=true;
        }
    };
    visit(xmlDocGetRootElement(doc));
    xmlFreeDoc(doc);
    if(exceeded) {error="DOCX text exceeds import limit";return false;}
    while(!text.empty() && (text.back()=='\n'||text.back()==' '||text.back()=='\r'))
        text.pop_back();
    if(text.empty()) {error="DOCX contains no text";return false;}
    return true;
}

static bool tickerImportDocx(const std::string& bytes, std::string& text, std::string& error) {
    std::string xml;
    return tickerExtractWordXml(bytes,xml,error) &&
           tickerParseWordText(xml,text,error);
}
} // namespace
