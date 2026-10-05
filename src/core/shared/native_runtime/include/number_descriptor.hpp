#pragma once
#include "engine.hpp"
#include <algorithm>
#include <cmath>
namespace astro {
// One checked scalar contract shared by the host boundary and native view.
struct NumberDescriptor {
  double value,minimum,maximum,step;
  bool available;
  std::string reason;
  static NumberDescriptor parse(const Value& v) {
    const auto& fields=v.record();
    for(const auto key:{"value","minimum","maximum","step","available","reason"})
      if(!fields.contains(key))throw std::runtime_error("Missing numeric UI descriptor field");
    NumberDescriptor d{v.at("value").number(),v.at("minimum").number(),v.at("maximum").number(),v.at("step").number(),v.at("available").boolean(),v.at("reason").string()};
    if(!std::isfinite(d.value)||!std::isfinite(d.minimum)||!std::isfinite(d.maximum)||!std::isfinite(d.step)
       ||d.minimum<0||d.maximum>1000000||d.maximum<=d.minimum||d.value<d.minimum||d.value>d.maximum
       ||d.step<=0||d.step>d.maximum-d.minimum)throw std::runtime_error("Invalid numeric UI descriptor");
    return d;
  }
  double unit(double number)const{return (number-minimum)/(maximum-minimum);}
  double quantize(double unit_value)const {
    auto raw=minimum+std::clamp(unit_value,0.0,1.0)*(maximum-minimum);
    return std::clamp(minimum+std::round((raw-minimum)/step)*step,minimum,maximum);
  }
};
}
