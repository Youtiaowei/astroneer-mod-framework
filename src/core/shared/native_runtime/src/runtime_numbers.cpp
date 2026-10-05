#include "runtime.hpp"
#include "number_descriptor.hpp"
#include <cmath>
namespace astro {
std::vector<ControlRow> Runtime::menu_rows() {
  auto rows=controls.rows();
  for(auto& row:rows) {
    auto it=modules_.find(row.id);
    if(it==modules_.end()||!it->second.instance||!it->second.number.set)continue;
    Value snapshot;
    auto& mi=it->second;
    auto code=call_module(row.id,mi,module::Operation::snapshot,{},mi.enabled,&snapshot);
    if(code||!accept_module_state(row.id,mi,snapshot))continue;
    if(snapshot.record().contains("number")&&!snapshot.at("number").null()) {
      row.number=snapshot.at("number");
      row.number.record()["available"]=row.number.at("available").boolean()&&ready&&mi.enabled&&!mi.error_code;
    }
  }
  return rows;
}
Value Runtime::set_number(const std::string& id,double value) {
  auto reply=record({{"id",id},{"applied",false},{"saved",false},{"blocked",true},
                     {"error",""},{"reason","wide.waiting_world"},{"value",Value{}}});
  auto it=modules_.find(id);
  if(it==modules_.end()||!it->second.instance||!it->second.number.set
     ||it->second.error_code)return reply;
  auto& mi=it->second;
  Value snapshot;
  auto snapshot_code=call_module(id,mi,module::Operation::snapshot,{},mi.enabled,&snapshot);
  if(snapshot_code||!accept_module_state(id,mi,snapshot)) {
    reply.record()["error"]="mod.runtime_error";return reply;
  }
  if(!snapshot.record().contains("number")||snapshot.at("number").null())return reply;
  const auto descriptor=NumberDescriptor::parse(snapshot.at("number"));
  reply.record()["value"]=descriptor.value;
  if(!ready||!mi.enabled||!std::isfinite(value))return reply;
  Value result;
  auto code=mi.number.set(mi.instance,value,false,&result);
  const auto fault=mi.engine_owner->fault();
  if(fault||(code&&code!=3)) {
    fail_module(id,mi,fault?fault:code);reply.record()["error"]="mod.runtime_error";return reply;
  }
  if(code==3){reply.record()["reason"]="wide.invalid_value";return reply;}
  try {
    bool applied=result.at("applied").boolean(),blocked=result.at("blocked").boolean();
    double actual=result.at("value").number();
    if(!std::isfinite(actual)||actual<descriptor.minimum||actual>descriptor.maximum||applied==blocked)
      throw std::runtime_error("Invalid numeric module result");
    result.record()["id"]=id;result.record()["saved"]=false;result.record()["error"]="";
    if(blocked)return result;
    auto preferences=controls.preferences();preferences.parameters=settings.read().parameters;
    preferences.parameters[id]=actual;
    bool saved=settings.save(preferences);
    result.record()["saved"]=saved;
    if(!saved)result.record()["error"]="设置保存失败";
    return result;
  } catch(const std::exception&) {
    fail_module(id,mi,3);reply.record()["error"]="mod.runtime_error";return reply;
  }
}
}
