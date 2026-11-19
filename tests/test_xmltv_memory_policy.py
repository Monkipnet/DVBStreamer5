#!/usr/bin/env python3
"""Scope-specific XMLTV source checks: large-feed memory policy (no media changes)."""
from pathlib import Path
s = Path("src/ExternalXmltv.cpp").read_text()
h = Path("src/ExternalXmltv.h").read_text()
m = Path("src/StreamManager.cpp").read_text()
assert 'wantedChannels_' in h
assert 'stream.epgEnabled && stream.epgMode != "dvb"' in s
assert 'wanted.count(programmeId)' in s
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
assert 'kProgramVersion = "10.8.580"' in Path("src/AppVersion.h").read_text()
print("PASS XMLTV selected-channel retention, refresh handling, move-only reconfigure and unchanged version")
