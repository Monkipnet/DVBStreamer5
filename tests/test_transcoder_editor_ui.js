// Run: node tests/test_transcoder_editor_ui.js
// UI-only regression guard: essential vs advanced controls and storage fidelity.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const source = fs.readFileSync(path.resolve(__dirname, '../src/http/HttpServerWebUi.inc'), 'utf8');
const scriptStart = source.indexOf('<script>');
const scriptEnd = source.indexOf('</script>', scriptStart);
assert(scriptStart >= 0 && scriptEnd > scriptStart);
const script = source.slice(scriptStart + 8, scriptEnd);
new Function(script); // syntax of the complete web UI

const ids = [
  'streamTranscodeVideoCodec', 'streamTranscodeVideoEncoder',
  'streamTranscodeResolution', 'streamTranscodeBitrate',
  'streamTranscodeFps', 'streamTranscodePreset', 'streamTranscodeDeinterlace',
  'streamTranscodeAudioCodec', 'streamTranscodeAudioBitrate',
  'streamTranscodeMultibitrate', 'streamTranscodeProfile',
  'streamTranscodeLevel', 'streamTranscodeGop',
  'streamTranscodeRefFrames', 'streamTranscodeAq',
  'streamTranscodeBFrames', 'streamTranscodeOpenGop'
];
for (const id of ids) {
  const occurrences = source.split('id="' + id + '"').length - 1;
  assert.equal(occurrences, 1, id + ' must appear exactly once in the editor');
}
assert(!source.includes('id="streamTranscodePixelFormat"'));
assert(!source.includes('id="streamTranscodeResizeMode"'));
assert(source.includes('id="streamTranscodeBFrames" disabled'));
assert(source.includes('id="streamTranscodeOpenGop" disabled'));
assert(source.includes('id="streamVideoAdvanced" class="edit-section edit-collapsible'));
assert(source.includes('class="edit-collapsible transcode-legacy-options"'));

// Main choices are before the Advanced drawer, not hidden behind it.
const start = source.indexOf('<small id="streamTranscodeTransferStatus"');
const advanced = source.indexOf('<details id="streamVideoAdvanced"', start);
const audio = source.indexOf('id="streamTranscodeAudioCodec"', start);
const preset = source.indexOf('id="streamTranscodePreset"', start);
const fps = source.indexOf('id="streamTranscodeFps"', start);
const profile = source.indexOf('id="streamTranscodeProfile"', start);
assert(start >= 0 && preset > start && fps > start);
assert(preset < advanced && fps < advanced && audio < advanced && profile > advanced);

// Keep existing codec/bitrate/storage keys and copy/paste handlers unchanged.
assert(script.includes('function copyTranscodeSettings()'));
assert(script.includes('function pasteTranscodeSettings()'));
for (const key of ['transcode_fps', 'transcode_preset', 'transcode_bframes',
                   'transcode_open_gop', 'transcode_ref_frames',
                   'transcode_video_bitrate']) {
  assert(script.includes(key + ':'), 'save field ' + key);
}
const bodyStart = script.indexOf('function updateTranscodeVideoControls() {');
const bodyEnd = script.indexOf('function updateTranscodeAudioControls()', bodyStart);
assert(bodyStart >= 0 && bodyEnd > bodyStart);
const func = script.slice(bodyStart, bodyEnd) + '\nreturn updateTranscodeVideoControls;';

function input(value) {
  return {
    value, disabled: false, checked: true,
    selectedOptions: [{disabled: false}],
    querySelector: () => ({disabled: false})
  };
}
const fields = {
  streamTranscodeVideoCodec: input('copy'),
  streamTranscodeVideoEncoder: input('nvenc'),
  streamTranscodeResolution: input('1280x720'),
  streamTranscodeBitrate: input('3500'),
  streamTranscodeFps: input('25'),
  streamTranscodePreset: input('balanced'),
  streamTranscodeDeinterlace: input('true'),
  streamTranscodeMultibitrate: input('false'),
  streamTranscodeProfile: input('auto'),
  streamVideoAdvanced: {style: {display: 'block'}}
};
const document = {getElementById: id => fields[id] ?? null};
const state = {transcoder: {
  nvenc_available: true, nvenc_hevc_available: true,
  intel_available: true, intel_hevc_available: true,
  x264_available: true, x265_available: true
}};
const refresh = new Function('document', 'state', func)(document,state);
refresh();
for (const key of ['streamTranscodeFps','streamTranscodePreset','streamTranscodeDeinterlace']) {
  assert(fields[key].disabled);
}
assert.equal(fields.streamVideoAdvanced.style.display,'none');
assert.equal(fields.streamTranscodePreset.value,'balanced');
fields.streamTranscodeVideoCodec.value='h264';
refresh();
for (const key of ['streamTranscodeFps','streamTranscodePreset','streamTranscodeDeinterlace']) {
  assert(!fields[key].disabled);
}
assert.equal(fields.streamVideoAdvanced.style.display,'block');
assert.equal(fields.streamTranscodePreset.value,'balanced');
console.log('Transcoder editor UI tests passed');
