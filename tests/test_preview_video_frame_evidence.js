'use strict';
// V503: browser frame confirmation is not a connection/CW/PES event.
const assert = require('node:assert/strict');
const preview = require('../web/preview/preview-player.js');
const emitted = [];
let currentVideo = null;
function element(tag) {
  const result = {
    tagName: tag, style: {}, paused: false, ended: false,
    videoWidth: tag === 'video' ? 1280 : 0, readyState: 4,
    addEventListener() {}, removeEventListener() {},
    append() {}, remove() {}, focus() {}, setAttribute() {},
    removeAttribute() {}, pause() {this.paused = true;},
    load() {}, closest() {return null;},
    requestVideoFrameCallback(cb) {this.frameCallback = cb;}
  };
  if (tag === 'video') currentVideo = result;
  return result;
}
global.document = {
  body: element('body'), activeElement: element('button'),
  createElement: element,
  addEventListener(){}, removeEventListener(){}
};
global.CustomEvent = class CustomEvent {
  constructor(type, config){this.type=type;this.detail=config.detail;}
};
global.window = {
  location: new URL('http://localhost:9000/'),
  addEventListener(){}, removeEventListener(){},
  dispatchEvent(event){emitted.push(event);}
};
const controller = preview.install({resolve:()=>Promise.resolve({sources:[]})});
controller.open('sid-test', 'test');
assert.equal(emitted.length, 0, 'Opening preview must not claim decode');
currentVideo.frameCallback(0, {presentedFrames: 0});
assert.equal(emitted.length, 0, 'No presented frames means no confirmation');
currentVideo.frameCallback(10, {presentedFrames: 2});
assert.equal(emitted.length, 1, 'Presented decoded video confirms preview');
assert.equal(emitted[0].type, 'dvbstreamer5:preview-video-frame');
assert.deepEqual(emitted[0].detail, {streamId:'sid-test'});
controller.close();
assert.equal(emitted.length, 1, 'Close never claims a frame');
controller.destroy();
console.log('V503 preview video-frame evidence regression passed');
