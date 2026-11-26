#include "CamSettingsPolicy.h"
#include <cassert>
#include <iostream>
int main(){
    Json::Value root(Json::objectValue), out;
    assert(!camsettings::savedClients(root,out));
    root["cam_clients"]=Json::Value(Json::arrayValue);
    assert(camsettings::savedClients(root,out)&&out.isArray()&&out.empty());
    Json::Value client(Json::objectValue), cfg(Json::objectValue);
    client["id"]="fixture";client["name"]="Fixture";client["max_services"]=60;
    client["backend_id"]="newcamd";cfg["user"]="fixture-user";cfg["pass"]="fixture-password";
    cfg["port"]=12345;cfg["host"]="192.0.2.1";cfg["des"]=std::string(28,'A');
    client["backend_config"]=camsettings::encode(cfg);
    root["cam_clients"].append(client);
    assert(camsettings::savedClients(root,out)&&out.size()==1&&out[0]==client);
    Json::Value parsed;assert(camsettings::decode(out[0]["backend_config"],parsed)&&parsed==cfg);
    root["cam_clients"].append(client);
    assert(!camsettings::savedClients(root,out));
    root["cam_clients"].resize(1);
    root["cam_clients"][0]["backend_config"]="invalid-json";
    assert(!camsettings::savedClients(root,out));
    root["cam_clients"][0]=client;root["cam_clients"][0]["backend_config"]=cfg;
    assert(camsettings::savedClients(root,out)&&out[0]["backend_config"]==cfg);
    root["cam_clients"][0]["max_services"]=65;
    assert(!camsettings::savedClients(root,out));
    root["cam_clients"]="broken";assert(!camsettings::savedClients(root,out));
    std::cout<<"PASS saved CAM snapshot: round-trip and fail-closed validation\n";
}
