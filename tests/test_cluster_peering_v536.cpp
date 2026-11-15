// V536 cluster credential store: exercise security constraints, persistence,
// and simulated peer inventory transitions without real DVB hardware.
#include "http/ClusterPeerStore.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <iostream>
#include <sys/stat.h>

using namespace dvbstreamer5::cluster;
int main() {
  assert(validOrigin("http://10.8.0.2:9000"));
  assert(validOrigin("http://172.16.3.2:9000"));
  assert(validOrigin("http://192.168.1.22:9000"));
  assert(validOrigin("https://edge.example.net:443"));
  assert(!validOrigin("http://8.8.8.8:9000"));
  assert(!validOrigin("http://blumen.zapto.org:9000"));
  assert(validOrigin("http://blumen.zapto.org:9000",true));
  assert(!validOrigin("http://localhost:9000",true));
  assert(!validOrigin("http://127.0.0.1:9000",true));
  assert(!validOrigin("http://blumen.zapto.org:9000/api",true));
  assert(!validOrigin("http://127.0.0.1:9000"));
  assert(!validOrigin("http://localhost:9000"));
  assert(!validOrigin("https://example.org@169.254.169.254"));
  assert(!validOrigin("https://example.net/api/state"));
  assert(!validOrigin("https://example.net?key=secret"));
  assert(!validOrigin("https://example.net/#frag"));
  assert(!validOrigin("https://example.net\r\nX-Key:hi"));
  assert(!validOrigin("https://example.net:70000"));
  const auto token=randomToken();
  assert(validToken(token));
  assert(tokenMatches(token,token));
  assert(!tokenMatches(token,token.substr(0,63)+"x"));
  assert(!tokenMatches(token,""));

  char raw[]="/tmp/dvbstreamer5-cluster-test-XXXXXX";
  const char* dir=::mkdtemp(raw);
  assert(dir);
  const std::filesystem::path directory(dir);
  Config config;
  assert(ensure(directory,config));
  assert(validToken(config.nodeToken));
  assert(config.peers.empty());
  struct stat st{};
  const auto path=filePath(directory);
  assert(::stat(path.c_str(),&st)==0);
  assert((st.st_mode & 0077)==0);
  const auto firstToken=config.nodeToken;
  assert(ensure(directory,config));
  assert(config.nodeToken==firstToken);

  Peer a{"home2","http://10.8.0.2:9000",randomToken()};
  Peer b{"iptv","https://iptv.example.net",randomToken()};
  Peer c{"svettranscoding","http://192.168.1.22:9000",randomToken()};
  assert(validPeer(a)&&validPeer(b)&&validPeer(c));
  b.verifyTlsPeer=false;
  assert(a.verifyTlsPeer && !b.verifyTlsPeer);
  Peer publicHttp{"Blumen","http://blumen.zapto.org:9000",randomToken()};
  assert(!validPeer(publicHttp));
  publicHttp.verifyTlsPeer=false;
  assert(validPeer(publicHttp));
  config.peers={a,b,c};
  assert(save(directory,config));
  Config reloaded;
  assert(load(directory,reloaded));
  assert(reloaded.peers.size()==3);
  assert(reloaded.peers[0].name=="home2");
  assert(reloaded.peers[1].name=="iptv");
  assert(reloaded.peers[2].name=="svettranscoding");
  assert(!reloaded.peers[1].verifyTlsPeer);
  assert(reloaded.peers[0].verifyTlsPeer);
  assert(publicPeers(reloaded,false)["peers"][1]["verify_tls"].asBool()==false);
  const auto publicView=publicPeers(reloaded,false);
  assert(publicView["peers"].size()==3);
  assert(!publicView.isMember("node_token"));
  for(const auto& peer:publicView["peers"])
    assert(!peer.isMember("token"));
  Config duplicate=reloaded;
  duplicate.peers.push_back(a);
  assert(!save(directory,duplicate));
  std::filesystem::remove_all(directory);
  std::cout<<"PASS: V536 cluster security, token and 3 virtual peers\n";
}
