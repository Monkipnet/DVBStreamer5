from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if text.count(old) != 1:
        raise SystemExit(f"{label}: expected exactly one match, got {text.count(old)}")
    return text.replace(old, new, 1)


dvb = Path("src/http/HttpServerDvbMedia.inc")
text = dvb.read_text(encoding="utf-8")

old = '''        item["configured_streams"] = item.get("configured_streams", 0).asUInt() + 1;
        if (toLower(config.activationMode) == "ondemand") {
            item["ondemand_streams"] = item.get("ondemand_streams", 0).asUInt() + 1;
        }
        if (!item.isMember("streams")) item["streams"] = Json::Value(Json::arrayValue);'''
new = '''        item["configured_streams"] = item.get("configured_streams", 0).asUInt() + 1;
        if (toLower(config.activationMode) == "ondemand") {
            item["ondemand_streams"] = item.get("ondemand_streams", 0).asUInt() + 1;
        }
        // Preserve one configured tune even while an OnDemand frontend is idle.
        // An active runtime consumer below overrides these values with the tune
        // that is physically in use right now.
        if (!item.isMember("frequency_khz")) {
            item["frequency_khz"] = params.frequencyKHz;
            item["symbol_rate"] = params.symbolRateK;
            item["polarity"] = params.polarity;
            item["delivery_system"] = params.deliverySystem;
            item["modulation"] = params.modulation;
            item["fec"] = params.fec;
            item["stream_id"] = params.streamId;
            item["diseqc_source"] = params.diseqcSource;
            item["lnb_lof1_khz"] = params.lnbLof1KHz;
            item["lnb_lof2_khz"] = params.lnbLof2KHz;
            item["lnb_slof_khz"] = params.lnbSlofKHz;
        }
        if (!item.isMember("streams")) item["streams"] = Json::Value(Json::arrayValue);'''
text = replace_once(text, old, new, "configured DVB usage insertion")

old = '''        item["delivery_system"] = params.deliverySystem;
        item["stream_id"] = params.streamId;
        item["diseqc_source"] = params.diseqcSource;'''
new = '''        item["delivery_system"] = params.deliverySystem;
        item["modulation"] = params.modulation;
        item["fec"] = params.fec;
        item["stream_id"] = params.streamId;
        item["diseqc_source"] = params.diseqcSource;
        item["lnb_lof1_khz"] = params.lnbLof1KHz;
        item["lnb_lof2_khz"] = params.lnbLof2KHz;
        item["lnb_slof_khz"] = params.lnbSlofKHz;'''
text = replace_once(text, old, new, "active DVB tune block")

start_marker = '                if (consumers > 0) {'
end_marker = '                if (found->second.isMember("streams")) {'
start = text.find(start_marker)
end = text.find(end_marker, start)
if start < 0 or end < 0 or end <= start:
    raise SystemExit("DVB response tune copy markers changed")
new_block = '''                if (found->second.isMember("frequency_khz")) {
                    adapter["frequency_khz"] = found->second.get("frequency_khz", 0);
                    adapter["symbol_rate"] = found->second.get("symbol_rate", 0);
                    adapter["polarity"] = found->second.get("polarity", "");
                    adapter["delivery_system"] = found->second.get("delivery_system", "");
                    adapter["modulation"] = found->second.get("modulation", "auto");
                    adapter["fec"] = found->second.get("fec", "auto");
                    adapter["stream_id"] = found->second.get("stream_id", -1);
                    adapter["diseqc_source"] = found->second.get("diseqc_source", -1);
                    adapter["lnb_lof1_khz"] = found->second.get("lnb_lof1_khz", 9750000);
                    adapter["lnb_lof2_khz"] = found->second.get("lnb_lof2_khz", 10600000);
                    adapter["lnb_slof_khz"] = found->second.get("lnb_slof_khz", 11700000);
                }
'''
text = text[:start] + new_block + text[end:]
dvb.write_text(text, encoding="utf-8")

ui = Path("src/http/HttpServerWebUi.inc")
text = ui.read_text(encoding="utf-8")
marker = "function updateSatelliteDeviceInfo(prefix=null) {"
if text.count(marker) != 1:
    raise SystemExit("updateSatelliteDeviceInfo marker changed")
helper = '''function applySelectedDvbFrontendTuneToForm() {
  const item = selectedDvbFrontend();
  const configured = Number(item?.configured_streams || 0);
  const consumers = Number(item?.consumers || 0);
  if (!item || (!(item.in_use === true) && configured <= 0 && consumers <= 0)) return false;
  const setValue = (id, value) => {
    const input = document.getElementById(id);
    if (input && value !== undefined && value !== null && value !== '') input.value = String(value);
  };
  const frequencyKHz = Number(item.frequency_khz || 0);
  const lof1KHz = Number(item.lnb_lof1_khz || 0);
  const lof2KHz = Number(item.lnb_lof2_khz || 0);
  const slofKHz = Number(item.lnb_slof_khz || 0);
  if (frequencyKHz > 0) setValue('satFrequency', frequencyKHz / 1000);
  if (Number(item.symbol_rate || 0) > 0) setValue('satSymbolRate', Number(item.symbol_rate));
  setValue('satPolarity', item.polarity || 'H');
  setValue('satDeliverySystem', item.delivery_system || 'dvb-s2');
  setValue('satModulation', item.modulation || 'auto');
  setValue('satFec', item.fec || 'auto');
  setValue('satDiseqc', Number(item.diseqc_source ?? -1));
  setValue('satStreamId', Number(item.stream_id ?? -1));
  if (lof1KHz > 0) setValue('satLof1', lof1KHz / 1000);
  if (lof2KHz > 0) setValue('satLof2', lof2KHz / 1000);
  if (slofKHz > 0) setValue('satSlof', slofKHz / 1000);
  applyTbsDriverModeUi();
  return true;
}
'''
text = text.replace(marker, helper + marker, 1)

old = '''  refreshSatelliteFrontendOptions();
  resetSatelliteServicesForTuneChange();
  updateSatelliteDeviceInfo();
  updateSatelliteSignal();
}
function satelliteFrontendChanged() {'''
new = '''  refreshSatelliteFrontendOptions();
  applySelectedDvbFrontendTuneToForm();
  resetSatelliteServicesForTuneChange();
  updateSatelliteDeviceInfo();
  updateSatelliteSignal();
}
function satelliteFrontendChanged() {'''
text = replace_once(text, old, new, "satelliteAdapterChanged")

old = '''  satelliteSignalPending = false;
  resetSatelliteServicesForTuneChange();
  applyTbsDriverModeUi();
  updateSatelliteDeviceInfo();
  updateSatelliteSignal();
}'''
new = '''  satelliteSignalPending = false;
  applySelectedDvbFrontendTuneToForm();
  resetSatelliteServicesForTuneChange();
  applyTbsDriverModeUi();
  updateSatelliteDeviceInfo();
  updateSatelliteSignal();
}'''
text = replace_once(text, old, new, "satelliteFrontendChanged")

old = '''    refreshSatelliteAdapterOptions(preferredAdapter, preferredFrontend);
    updateSatelliteDeviceInfo();
    updateSatelliteSignal();'''
new = '''    refreshSatelliteAdapterOptions(preferredAdapter, preferredFrontend);
    applySelectedDvbFrontendTuneToForm();
    updateSatelliteDeviceInfo();
    updateSatelliteSignal();'''
text = replace_once(text, old, new, "loadSatelliteAdapters selected block")
ui.write_text(text, encoding="utf-8")

print("V10.8.155 occupied DVB tune editor patch applied")
