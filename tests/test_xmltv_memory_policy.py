#!/usr/bin/env python3
"""Scope-specific XMLTV source checks: large-feed memory policy (no media changes)."""
from pathlib import Path
s = Path("src/ExternalXmltv.cpp").read_text()
h = Path("src/ExternalXmltv.h").read_text()
m = Path("src/StreamManager.cpp").read_text()
assert 'wantedChannelsBySource_' in h
assert 'std::map<std::string, std::set<std::string>> wantedBySource;' in s
assert 'wantedBySource[source.id].insert(stream.xmltvChannelId)' in s
assert 'wantedBySource[stream.xmltvSourceId].insert(stream.xmltvChannelId)' in s
assert 'wantedBySource[service.xmltvSourceId].insert(service.xmltvChannelId)' in s
assert 'auto oldWanted = std::move(wantedChannelsBySource_);' in s
assert 'const auto& wanted = wantedChannelsBySource_.at(cfg.id);' in s
assert 'const auto wanted = wantedChannelsBySource_.find(selected.id);' in s
assert 'inProgramme = wanted.count(programmeId) != 0;' in s
assert 'if (inProgramme) {' in s
assert 'if (data.channels.empty() && totalEvents == 0)' in s
assert 'wantedChannels_' not in h
assert 'stream.epgMode == "dvb"' in s
assert 'stream.epgEnabled' in s
assert 'inProgramme = wanted.count(programmeId) != 0;' in s
assert 'wanted.find(channel) == wanted.end()' in s
assert 'data = std::move(it->second);' in s
assert 'data = it->second;' not in s
assert 'loaded.programmes.erase(itEvent)' in s
assert 'source.queued = true' in s
assert 'loaded.settings = selected' in s
# Source manager now receives external WISI EPG services in addition to tile streams.
# Normalize whitespace so harmless formatting does not break this policy test.
compact = " ".join(m.split())
assert 'std::make_unique<ExternalXmltvManager>(cfg.config.xmltvSources,' in compact
assert 'cfg.config.streams, cfg.config.epgUdpOutputs)' in compact
assert 'externalXmltvManager_->snapshot(service.xmltvSourceId,' in compact
assert 'externalXmltvManager_->configure(configManager.config.xmltvSources,' in m
import re
version = re.search(r'kProgramVersion = "10\.8\.(\d+)"', Path("src/AppVersion.h").read_text())
assert version and int(version.group(1)) >= 580
print("PASS XMLTV selected-channel retention, refresh handling, move-only reconfigure and unchanged version")
