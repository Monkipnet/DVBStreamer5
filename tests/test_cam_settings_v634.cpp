#include "CamSettingsPolicy.h"
#include <cassert>
#include <iostream>
int main(){
    Json::Value prior(Json::objectValue);
    prior["host"]="192.0.2.1";prior["port"]=12345;prior["user"]="fixture-user";prior["pass"]="fixture-password";prior["des"]=std::string(28,'A');prior["extra"]=42;
    Json::Value patch(Json::objectValue),out;
    patch["user"]="fixture-renamed";patch["pass"]="";
    assert(camsettings::merge("newcamd",patch,prior,out));
    assert(out["user"]=="fixture-renamed" && out["pass"]=="fixture-password" && out["extra"]==42);
    assert(out["host"]==prior["host"] && out["port"]==prior["port"] && out["des"]==prior["des"]);
    patch["pass"]="fixture-replaced";patch["port"]=23456;
    assert(camsettings::merge("newcamd",patch,prior,out));
    assert(out["pass"]=="fixture-replaced" && out["port"]==23456);
    Json::Value reload;
    assert(camsettings::decode(camsettings::encode(out),reload) && reload==out);
    assert(camsettings::readerChanged("newcamd",camsettings::encode(prior),"newcamd",camsettings::encode(out)));
    assert(!camsettings::readerChanged("newcamd",camsettings::encode(out),"newcamd",camsettings::encode(reload)));
    patch["port"]=65536;
    assert(!camsettings::merge("newcamd",patch,prior,out));
    patch=prior;patch["protocol"]="newcamd";patch["caid"]="1234";patch["ecm_pid"]=321;
    assert(camsettings::merge("tsdecrypt",patch,Json::Value(),out));
    assert(out["protocol"]=="NEWCAMD" && out["caid"]=="1234" && out["ecm_pid"]==321);
    patch["ecm_pid"]=8191;assert(!camsettings::merge("tsdecrypt",patch,Json::Value(),out));
    patch=prior;patch["pass"]="";assert(!camsettings::merge("newcamd",patch,Json::Value(),out));
    patch=prior;patch.removeMember("user");patch.removeMember("pass");patch["username"]="legacy";patch["password"]="fixture";
    assert(camsettings::merge("newcamd",patch,Json::Value(),out));
    assert(out["user"]=="legacy" && out["pass"]=="fixture");
    std::cout<<"PASS CAM settings merge, validation, reader change and JSON round-trip\n";
}
