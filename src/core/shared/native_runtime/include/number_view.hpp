#pragma once
#include "umg.hpp"
#include "localization.hpp"
#include "number_descriptor.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
namespace astro {
class NumberView {
  Object slider,label,note,reason;
  double presented=1;
  double written_thumb=0;
  std::string message;
  bool last_available=false;
  std::string last_reason;
  static std::string value_text(const std::string& language,double value) {
    std::ostringstream n;n<<std::fixed<<std::setprecision(2)<<value;
    auto text=i18n::text(language,"wide.range_template");
    auto p=text.find("{value}");if(p==std::string::npos)throw std::runtime_error("Numeric translation placeholder missing");
    text.replace(p,std::string("{value}").size(),n.str());return text;
  }
public:
  void mount(UMG& ui,Object items,const std::string& id,const Value& number,const std::string& language) {
    auto d=NumberDescriptor::parse(number);presented=d.value;
    last_available=d.available;last_reason=d.reason;message.clear();
    auto column=ui.make("VerticalBox",id+"_RangeColumn");
    label=ui.text(value_text(language,d.value),16,id+"_RangeValue");
    ui.add(column,ui.fixed(label,24,id+"_RangeValueSize"));
    slider=ui.make("Slider",id+"_RangeSlider");
    ui.e.set(slider,"IsFocusable",true);
    written_thumb=d.unit(d.value);
    ui.e.call(slider,"SetValue",{written_thumb});
    ui.e.call(slider,"SetIsEnabled",{d.available});
    ui.add(column,ui.fixed(slider,32,id+"_RangeSliderSize"));
    note=ui.text(i18n::text(language,"wide.baseline"),12,id+"_RangeNote");
    ui.tone(note,color(.35,.43,.47));ui.add(column,ui.fixed(note,24,id+"_RangeNoteSize"));
    reason=ui.text(i18n::text(language,d.reason),12,id+"_RangeReason");
    ui.add(column,ui.fixed(reason,24,id+"_RangeReasonSize"));
    auto slot=ui.add(items,column);ui.e.call(slot,"SetPadding",{padding(4,0,4,8)});
  }
  void update(UMG& ui,const Value& number,bool interact,const std::string& language,const std::function<Value(double)>& change) {
    auto d=NumberDescriptor::parse(number);
    if(d.available!=last_available||d.reason!=last_reason||d.value!=presented)message.clear();
    last_available=d.available;last_reason=d.reason;
    auto raw=ui.e.call(slider,"GetValue").number();
    if(!std::isfinite(raw))throw std::runtime_error("Invalid native slider value");
    auto requested=d.quantize(raw);
    // Compare against the last position actually written to this widget, not
    // the previous value interpreted through a newly changed descriptor range.
    const bool thumb_moved=std::abs(raw-written_thumb)>1e-6;
    if(interact&&d.available&&change&&thumb_moved&&std::abs(requested-presented)>1e-6) {
      try {
        auto result=change(requested);
        const auto actual=result.at("value").number();
        const bool applied=result.at("applied").boolean(),blocked=result.at("blocked").boolean();
        if(!std::isfinite(actual)||actual<d.minimum||actual>d.maximum||applied==blocked)
          throw std::runtime_error("Invalid slider reply");
        auto next_message=result.record().contains("error")?result.at("error").string():"";
        if(blocked&&next_message.empty())next_message=result.at("reason").string();
        // Commit only after every borrowed reply field has been checked.
        d.value=actual;message=std::move(next_message);
      }catch(...){message="mod.runtime_error";}
    }
    presented=d.value;
    const auto target=d.unit(presented);
    ui.e.call(slider,"SetValue",{target});written_thumb=target;
    ui.e.call(slider,"SetIsEnabled",{interact&&d.available});
    ui.e.call(label,"SetText",{value_text(language,presented)});
    ui.e.call(note,"SetText",{i18n::text(language,"wide.baseline")});
    ui.e.call(reason,"SetText",{i18n::text(language,message.empty()?d.reason:message)});
    ui.tone(reason,message.empty()?color(.35,.43,.47):color(.64,.12,.07));
  }
  std::string widget(Engine& e)const{return e.valid(slider)?e.name(slider):"";}
};
}
