#!/usr/bin/env python3
"""Compare live DVBStreamer5 to FFmpeg SW/HW encoding on a fixed local TS clip."""
import json
import os
import re
import shutil
import signal
import subprocess
import time
from pathlib import Path
from statistics import mean

SID = 'stream-1794476735932'
CONFIG = '/etc/dvbstreamer5/dvbstreamer5-config.json'
SAMPLE = '/tmp/dvb489_benchmark_input.ts'
RUN_SECONDS = 40
HZ = os.sysconf('SC_CLK_TCK')


def proc_stats(pid):
    try:
        stat = Path(f'/proc/{pid}/stat').read_text()
        f = stat[stat.rfind(')')+2:].split()
        ticks = (int(f[11]) + int(f[12])) / HZ
        kb = 0
        for row in Path(f'/proc/{pid}/status').read_text().splitlines():
            if row.startswith('VmRSS:'):
                kb = int(row.split()[1]); break
        return ticks, kb / 1024
    except (OSError, ValueError, IndexError):
        return None


def monitor(pids, seconds, child=None):
    result = {name: [] for name in pids}
    last = {name: proc_stats(pid) for name, pid in pids.items()}
    at = time.monotonic()
    end = at + seconds
    while time.monotonic() < end:
        if child is not None and child.poll() is not None:
            break
        time.sleep(1)
        now = time.monotonic()
        dt = now - at
        for name, pid in pids.items():
            cur, prev = proc_stats(pid), last[name]
            if cur is not None and prev is not None and dt > 0:
                result[name].append((max(0, 100 * (cur[0]-prev[0]) / dt), cur[1]))
            last[name] = cur
        at = now
    return result


def show_results(label, samples):
    print('\n===== ' + label + ' =====')
    for key, records in samples.items():
        if not records:
            print(key + ': no samples')
            continue
        print('%s: CPU avg %.1f%%, max %.1f%% | RSS avg %.1f MiB, peak %.1f MiB' %
              (key, mean(x[0] for x in records), max(x[0] for x in records),
               mean(x[1] for x in records), max(x[1] for x in records)))


def run(cmd, timeout, log):
    with open(log, 'w') as dst:
        try:
            return subprocess.run(cmd, stdin=subprocess.DEVNULL,
                                  stdout=dst, stderr=subprocess.STDOUT,
                                  timeout=timeout, check=False).returncode
        except subprocess.TimeoutExpired:
            return 124


def progress(path):
    d = {}
    if Path(path).exists():
        for line in Path(path).read_text(errors='replace').splitlines():
            if '=' in line:
                k, v = line.split('=', 1)
                d[k] = v
    return d


if shutil.which('ffmpeg') is None or shutil.which('ffprobe') is None:
    raise SystemExit('ERROR: ffmpeg and ffprobe required')

pid = int(subprocess.check_output([
    'systemctl','show','-p','MainPID','--value','dvbstreamer5.service'
], text=True).strip())
if pid < 1 or not Path(f'/proc/{pid}').exists():
    raise SystemExit('ERROR: DVBStreamer5 service is not running')

cfg = json.load(open(CONFIG))
s = next((x for x in cfg['streams'] if x.get('id') == SID), None)
if not s:
    raise SystemExit('ERROR: tile not found')
url = s.get('input_uri','')
if not url.startswith(('http://','https://')):
    raise SystemExit('ERROR: test expects HTTP/HTTPS input')

m = re.match(r'^(\d+)x(\d+)', s.get('transcode_resolution','2560x1440'))
if not m:
    raise SystemExit('ERROR: resolution invalid')
w, h = map(int, m.groups())
fps = float(s.get('transcode_fps', 25))
video_bps = int(s.get('transcode_video_bitrate', 9000000))
audio_bps = int(s.get('transcode_audio_bitrate', 192000))
if s.get('cbr') and int(s.get('target_bitrate',0)) >= 1000000:
    target = int(s['target_bitrate'])
    reserve = max(500000, audio_bps + target // 20)
    budget = target - reserve if target > reserve else target // 2
    video_bps = max(500000, min(video_bps, budget))

print('===== CONFIG =====')
print('DVBStreamer5 PID:', pid)
print('Configured DVB encoder:', s.get('transcode_video_encoder','auto'))
print('Video:', f'{w}x{h} @ {fps:g} fps / {video_bps//1000} kbps')
print('Audio:', s.get('transcode_audio_codec','aac'), f'{audio_bps//1000} kbps')
print('Input capture: 9 s of original HTTP MPEG-TS; address is not printed')
print('Each FFmpeg trial: 40 s wall time; real-time paced loop of local sample')
print('Audio/transcode filters approx match native; codecs/presets differ')

print('\n===== DVBStreamer5 ALONE (15s) =====', flush=True)
show_results('NATIVE BASELINE', monitor({'DVBStreamer5':pid}, 15))

print('\n===== CAPTURING SINGLE LOCAL SOURCE CLIP =====', flush=True)
rc = run(['ffmpeg','-hide_banner','-nostdin','-v','error',
          '-rw_timeout','15000000','-i',url, '-t','9',
          '-map','0:v:0','-map','0:a:0?', '-c','copy',
          '-f','mpegts','-y',SAMPLE], 40, '/tmp/dvb489_capture.log')
size = Path(SAMPLE).stat().st_size if Path(SAMPLE).exists() else 0
print('Capture exit:',rc,'size:', round(size/1024/1024,1),'MiB')
if size < 1_000_000:
    print('Capture unsuccessful; see /tmp/dvb489_capture.log')
    raise SystemExit(2)

probe = subprocess.run(['ffprobe','-v','error','-show_entries',
    'format=duration','-of','default=noprint_wrappers=1:nokey=1',SAMPLE],
    text=True, capture_output=True)
try:
    duration = float(probe.stdout.strip())
except ValueError:
    duration = 0
print('Capture duration:',round(duration,2),'s')
if duration < 5:
    raise SystemExit('ERROR: source clip is too short for a valid comparison')

vf = []
if s.get('transcode_deinterlace',True) and s.get('transcode_deinterlace_mode','legacy') != 'off':
    vf.append('yadif=mode=send_frame:parity=auto:deint=interlaced')
vf += [f'scale={w}:{h}:flags=bilinear', f'fps={fps:g}']
print('Common video filters:',','.join(vf))

encoders = subprocess.check_output(['ffmpeg','-hide_banner','-encoders'],
                                   text=True, stderr=subprocess.DEVNULL)
node = next((str(p) for p in sorted(Path('/dev/dri').glob('renderD*'))), None) \
       if Path('/dev/dri').exists() else None
print('GPU render node:',node or 'none')
if shutil.which('nvidia-smi'):
    subprocess.run(['nvidia-smi','-L'], check=False)

variants = [('CPU libx264 fast',[],vf+['format=yuv420p'],
             ['-c:v','libx264','-preset','fast'])]
if re.search(r'\bh264_nvenc\b',encoders):
    variants.append(('NVIDIA NVENC (encode only)',[],vf+['format=yuv420p'],
                     ['-c:v','h264_nvenc','-preset','p4']))
if node and re.search(r'\bh264_vaapi\b',encoders):
    variants.append(('VAAPI (encode only)', ['-vaapi_device',node],
                     vf+['format=nv12','hwupload'], ['-c:v','h264_vaapi']))
if node and re.search(r'\bh264_qsv\b',encoders):
    variants.append(('Intel QSV (encode only)', [],
                     vf+['format=nv12'], ['-c:v','h264_qsv','-preset','medium']))

for idx,(name,hwopts,filters,encoder) in enumerate(variants,1):
    key = 'v489_ffmpeg_test_' + str(idx)
    log = '/tmp/' + key + '.log'
    progress_path = '/tmp/' + key + '.progress'
    Path(progress_path).unlink(missing_ok=True)
    cmd = ['ffmpeg','-hide_banner','-nostdin','-v','error',
           '-stats_period','1','-progress',progress_path] + hwopts + [
           '-re','-stream_loop','-1','-i',SAMPLE,
           '-map','0:v:0','-map','0:a:0?',
           '-vf',','.join(filters)] + encoder + [
           '-b:v',str(video_bps),'-g',str(round(fps*2)),'-bf','0',
           '-c:a','aac','-ar','48000','-ac','2','-b:a',str(audio_bps),
           '-f','mpegts','-y','/dev/null']
    print('\n===== RUN '+name+' =====', flush=True)
    with open(log,'w') as lf:
        child = subprocess.Popen(cmd,stdin=subprocess.DEVNULL,
                                 stdout=lf,stderr=subprocess.STDOUT,
                                 start_new_session=True)
        t0 = time.monotonic()
        readings = monitor({'DVBStreamer5':pid,name:child.pid}, RUN_SECONDS,child)
        elapsed = time.monotonic()-t0
        stopped_by_us = child.poll() is None
        if stopped_by_us:
            os.killpg(child.pid,signal.SIGINT)
        try:
            child.wait(timeout=8)
        except subprocess.TimeoutExpired:
            os.killpg(child.pid,signal.SIGKILL)
            child.wait()
    show_results(name,readings)
    p = progress(progress_path)
    frames = int(p.get('frame','0') or '0')
    print('Elapsed: %.1fs | frames: %d | wall-FPS: %.2f | reported speed: %s | exit: %s' %
          (elapsed,frames,frames/max(0.01,elapsed),p.get('speed','?'),child.returncode))
    print('Ended by benchmark timer:',stopped_by_us)
    if elapsed < RUN_SECONDS-3 or frames < 100:
        print('WARNING: incomplete or failed trial. Last log lines:')
        err = Path(log).read_text(errors='replace').replace(url,'[SOURCE_URL]')
        print('\n'.join(err.splitlines()[-12:])[:1800])
    print('Log:',log)
print('\n===== COMPLETE =====')
print('100%% CPU = one logical CPU core. DVB RSS/CPU include other tiles.')
print('FFmpeg comparisons use the same input clip and approximate settings,')
print('not identical video quality or a fully hardware decode/scale pipeline.')
