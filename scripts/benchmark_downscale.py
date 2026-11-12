#!/usr/bin/env python3
"""Offline 1080p->720p/540p Native vs FFmpeg CPU/RAM comparison.

Does not modify configuration, stop the service, or publish an output stream.
For each target, Native and FFmpeg read the same captured MPEG-TS file.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from statistics import mean

SID = 'stream-1794476735932'
CONFIG = '/etc/dvbstreamer5/dvbstreamer5-config.json'
HZ = os.sysconf('SC_CLK_TCK')


def proc_stat(pid):
    try:
        data = Path(f'/proc/{pid}/stat').read_text()
        fields = data[data.rfind(')') + 2:].split()
        cpu = (int(fields[11]) + int(fields[12])) / HZ
        rss = 0
        for row in Path(f'/proc/{pid}/status').read_text().splitlines():
            if row.startswith('VmRSS:'):
                rss = int(row.split()[1]) / 1024
                break
        return cpu, rss
    except (OSError, ValueError, IndexError):
        return None


def trial(cmd, name, timeout, log, environment=None):
    print('\n===== %s =====' % name, flush=True)
    with open(log, 'w') as output:
        p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                             stdout=output, stderr=subprocess.STDOUT,
                             start_new_session=True, env=environment)
        started = last_time = time.monotonic()
        last = proc_stat(p.pid)
        readings = []
        forced = False
        while True:
            if p.poll() is not None:
                break
            if time.monotonic() - started > timeout:
                forced = True
                os.killpg(p.pid, signal.SIGTERM)
                break
            time.sleep(0.5)
            now = time.monotonic()
            current = proc_stat(p.pid)
            if current and last:
                dt = now - last_time
                if dt > 0:
                    readings.append((max(0.0, 100.0 * (current[0]-last[0])/dt),
                                     current[1]))
            last, last_time = current, now
        try:
            rc = p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            rc = p.wait()
            forced = True
        elapsed = time.monotonic() - started
    logtext = Path(log).read_text(errors='replace')
    result = dict(name=name, exit=rc, timeout=forced,
                  elapsed_s=round(elapsed, 2), log=log,
                  cpu_avg=round(mean(v[0] for v in readings), 1) if readings else None,
                  cpu_peak=round(max(v[0] for v in readings), 1) if readings else None,
                  rss_avg_mib=round(mean(v[1] for v in readings), 1) if readings else None,
                  rss_peak_mib=round(max(v[1] for v in readings), 1) if readings else None)
    perf = re.findall(r'NATIVE VIDEO ENCODE PERF stream_id=offline-benchmark\s+frames=(\d+)\s+fps=(\d+)', logtext)
    if perf:
        result['native_perf_windows'] = len(perf)
        result['native_reported_fps'] = [int(x[1]) for x in perf]
    status = re.search(r'NATIVE_BENCH_RESULT (.*)', logtext)
    eof_summary = re.search(r'NATIVE VIDEO EOF SUMMARY stream_id=offline-benchmark (.*)', logtext)
    if eof_summary:
        for key in ('input_video_samples', 'decoded_video_frames', 
                    'raw_published_frames', 'muxed_video_samples',
                    'startup_video_dropped', 'fps_gate_dropped', 'mux_video_dropped'):
            match = re.search(r'\b' + re.escape(key) + r'=(\d+)', eof_summary.group(1))
            if match:
                result[key] = int(match.group(1))
    if status:
        result['native_status'] = status.group(1)
        match = re.search(r'\boutput_video_samples=(\d+)', status.group(1))
        if match:
            result['output_video_samples'] = int(match.group(1))
        dropped = re.search(r'\bfps_gate_drop=(\d+)', status.group(1))
        if dropped:
            result['fps_gate_drop'] = int(dropped.group(1))
    progress = re.findall(r'(?m)^frame=(\d+)$', logtext)
    if progress:
        result['ffmpeg_frames'] = int(progress[-1])
        result['ffmpeg_fps_per_wall_s'] = round(int(progress[-1])/max(0.1, elapsed), 2)
    has_output = bool(status) if name.startswith('Native ') else bool(progress)
    result['valid'] = rc == 0 and not forced and has_output
    if name.startswith('Native ') and not perf:
        result['valid'] = False
    def fmt(v):
        return '—' if v is None else str(v)
    print('exit=%s elapsed=%ss CPU=%s%% (peak=%s%%) RSS=%s MiB (peak=%s MiB)' %
          (rc, result['elapsed_s'], fmt(result['cpu_avg']), fmt(result['cpu_peak']),
           fmt(result['rss_avg_mib']), fmt(result['rss_peak_mib'])))
    if perf:
        print('Native encode FPS windows:', result['native_reported_fps'],
              '| input PES:', result.get('input_video_samples', '?'),
              '| decoded:', result.get('decoded_video_frames', '?'),
              '| RAW published:', result.get('raw_published_frames', '?'),
              '| muxed video:', result.get('muxed_video_samples', '?'),
              '| startup drops:', result.get('startup_video_dropped', '?'),
              '| output video samples:', result.get('output_video_samples', '?'),
              '| FPS gate drops:', result.get('fps_gate_drop', '?'),
              '| status:', result.get('native_status', 'missing'))
    elif progress:
        print('FFmpeg frames:', result['ffmpeg_frames'],
              '| fps over elapsed:', result['ffmpeg_fps_per_wall_s'])
    if not result['valid']:
        print('WARNING: trial failed or incomplete. Last log lines:')
        print('\n'.join(logtext.splitlines()[-12:])[:2500])
    print('Log:', log)
    return result


def probe(path):
    p = subprocess.run([
        'ffprobe', '-v', 'error', '-show_streams', '-show_format',
        '-of', 'json', str(path)], capture_output=True, text=True, check=True)
    d = json.loads(p.stdout)
    video = next(x for x in d['streams'] if x['codec_type'] == 'video')
    duration = float(d['format']['duration'])
    return (int(video['width']), int(video['height']), duration,
            video.get('codec_name', 'unknown'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', default='build-v491',
                        help='Existing CMake build directory (default: build-v491)')
    parser.add_argument('--seconds', type=int, default=25,
                        help='Duration of one captured source TS (15..90)')
    parser.add_argument('--input-ts', default='',
                        help='Optional pre-captured MPEG-TS file; avoids a second live request')
    parser.add_argument('--targets', default='1280x720,960x540',
                        help='Comma-separated targets (default: 1280x720,960x540)')
    parser.add_argument('--with-qsv', action='store_true',
                        help='Also try QSV backends')
    args = parser.parse_args()
    if not 15 <= args.seconds <= 90:
        parser.error('--seconds must be between 15 and 90')
    if not shutil.which('ffmpeg') or not shutil.which('ffprobe'):
        raise SystemExit('ERROR: ffmpeg/ffprobe not found')
    root = Path(__file__).resolve().parents[1]
    native = root / args.build_dir / 'dvbstreamer5_downscale_benchmark'
    if not native.is_file() or not os.access(native, os.X_OK):
        raise SystemExit('ERROR: missing native test executable %s\n'
                         'Run: cmake --build %s --target dvbstreamer5_downscale_benchmark --parallel 2' %
                         (native, args.build_dir))
    config = json.loads(Path(CONFIG).read_text())
    stream = next((s for s in config['streams'] if s.get('id') == SID), None)
    if stream is None:
        raise SystemExit('ERROR: SportTV_HD stream id not in config')

    targets = []
    bitrates = {(1280,720):3000000, (960,540):1800000, (1920,1080):5000000}
    for entry in args.targets.split(','):
        m = re.fullmatch(r'(\d+)x(\d+)', entry.strip())
        if not m:
            parser.error('Invalid resolution: '+entry)
        width, height = map(int, m.groups())
        if (width, height) not in bitrates:
            parser.error('Supported targets: 1280x720, 960x540, 1920x1080')
        if (width,height) not in targets:
            targets.append((width,height))
    url = stream.get('input_uri', '')
    if not args.input_ts and not url.startswith(('http://', 'https://')):
        raise SystemExit('ERROR: HTTP input URL not available')

    env = os.environ.copy()
    env['MALLOC_ARENA_MAX'] = '2'  # Same allocator policy for both processes.
    enc = subprocess.check_output(['ffmpeg','-hide_banner','-encoders'],
                                  text=True, stderr=subprocess.DEVNULL)
    render = next((str(x) for x in sorted(Path('/dev/dri').glob('renderD*'))), None)
    nvidia = (Path('/proc/driver/nvidia/version').exists() and
              (Path('/usr/lib/x86_64-linux-gnu/libcuda.so.1').exists()
               or Path('/lib/x86_64-linux-gnu/libcuda.so.1').exists()
               or shutil.which('nvidia-smi') is not None))
    results = []
    with tempfile.TemporaryDirectory(prefix='dvb-downscale-') as work:
        tmp = Path(work)
        sample = Path(args.input_ts) if args.input_ts else tmp / 'capture.ts'
        if not args.input_ts:
            print('===== CAPTURE SOURCE WITHOUT TOUCHING LIVE TILE =====',flush=True)
            with open(tmp / 'capture.log', 'w') as log:
                cap = subprocess.run([
                    'ffmpeg','-hide_banner','-nostdin','-v','error',
                    '-rw_timeout','15000000','-i',url,
                    '-t',str(args.seconds),'-map','0:v:0','-map','0:a:0?',
                    '-c','copy','-f','mpegts','-y',str(sample)
                ],stdout=log,stderr=subprocess.STDOUT,timeout=args.seconds+25)
            if cap.returncode:
                raise SystemExit('ERROR: source capture failed, exit=%s' % cap.returncode)
        source_width, source_height, duration, codec = probe(sample)
        if duration < 10 or sample.stat().st_size < 1000000:
            raise SystemExit('ERROR: captured TS too short')
        if sample.stat().st_size % 188 != 0:
            raise SystemExit('ERROR: TS is not aligned on 188-byte packets')
        print('Source: %dx%d %s, %.1fs, %.1f MiB' %
              (source_width, source_height, codec, duration,
               sample.stat().st_size / 1048576))
        # Count *decoded source frames*, rather than relying on MPEG-TS
        # packet counts (which may be unavailable as nb_read_packets).
        # FFmpeg's fps=25 filter can duplicate or drop frames, so this is
        # essential to separate its CFR conversion from actual decoder loss.
        source_frames_probe = subprocess.run([
            'ffprobe', '-v', 'error', '-count_frames',
            '-select_streams', 'v:0',
            '-show_entries', 'stream=nb_read_frames',
            '-of', 'json', str(sample)],
            check=True, capture_output=True, text=True)
        source_frame_streams = json.loads(source_frames_probe.stdout).get('streams', [])
        try:
            source_frames = int(source_frame_streams[0]['nb_read_frames'])
        except (KeyError, ValueError, IndexError):
            source_frames = None
        print('Source decoded H264 frames (ffprobe):', source_frames)
        print('Video decode: CPU FFmpeg (native and FFmpeg); audio: copy')
        print('Output: discarded, nothing is published to network')
        print('Allocator: MALLOC_ARENA_MAX=2 for BOTH processes')
        print('Live service stays unchanged; benchmark may temporarily increase host CPU/GPU load')
        for width, height in targets:
            bitrate = bitrates[(width,height)]
            raster = f'{width}x{height}'
            print('\n################ OUTPUT %s, %d kbps ################' %
                  (raster, bitrate//1000),flush=True)

            native_variants = ['cpu']
            if render:
                native_variants.append('vaapi')
            if nvidia:
                native_variants.append('nvenc')
            if args.with_qsv and render:
                native_variants.append('qsv')
            for backend in native_variants:
                name = f'Native {backend} {raster}'
                cmd = [str(native),str(sample),f'{duration:.3f}',
                       str(width),str(height),backend,str(bitrate)]
                log = '/tmp/dvb_downscale_' + raster + '_native_' + backend + '.log'
                result = trial(cmd,name,duration+18,log,env)
                result.update(resolution=raster,backend=backend,implementation='Native',
                              source_decoded_frames=source_frames)
                results.append(result)

            vf = [
                'yadif=mode=send_frame:parity=auto:deint=interlaced',
                f'scale={raster}:flags=bilinear', 'fps=25'
            ]
            ffmpeg_variants = [('libx264',['format=yuv420p'],
                               ['-c:v','libx264','-preset','fast'],[])]
            if re.search(r'\blibopenh264\b', enc):
                ffmpeg_variants.append(('libopenh264',['format=yuv420p'],
                     ['-c:v','libopenh264'],[]))
            if render and re.search(r'\bh264_vaapi\b',enc):
                ffmpeg_variants.append(('vaapi',['format=nv12','hwupload'],
                     ['-c:v','h264_vaapi'],['-vaapi_device',render]))
            if nvidia and re.search(r'\bh264_nvenc\b',enc):
                ffmpeg_variants.append(('nvenc',['format=yuv420p'],
                     ['-c:v','h264_nvenc','-preset','p4'],[]))
            if args.with_qsv and render and re.search(r'\bh264_qsv\b',enc):
                ffmpeg_variants.append(('qsv',['format=nv12'],
                     ['-c:v','h264_qsv','-preset','medium'],[]))

            for backend,post,encoder,hw_opts in ffmpeg_variants:
                name=f'FFmpeg {backend} {raster}'
                ff_log='/tmp/dvb_downscale_'+raster+'_ffmpeg_'+backend+'.log'
                cmd=['ffmpeg','-hide_banner','-nostdin','-v','error',
                     '-stats_period','1','-progress','pipe:1'] + hw_opts + [
                     '-re','-hwaccel','none','-i',str(sample),
                     '-map','0:v:0','-map','0:a:0?',
                     '-vf',','.join(vf+post)] + encoder + [
                     '-b:v',str(bitrate),'-maxrate',str(bitrate),
                     '-bufsize',str(bitrate*2),'-g','50','-bf','0',
                     '-c:a','copy','-f','mpegts','-y','/dev/null']
                result=trial(cmd,name,duration+18,ff_log,env)
                result.update(resolution=raster,backend=backend,implementation='FFmpeg',
                              source_decoded_frames=source_frames)
                results.append(result)

    dest=Path('/tmp/dvb_downscale_results.json')
    dest.write_text(json.dumps(results,ensure_ascii=False,indent=2))
    print('\n===== RESULTS (child process only, same sample) =====')
    print('%-9s %-8s %-12s %8s %9s %8s %9s %6s' %
          ('Output','Engine','Backend','CPU avg','RSS avg','Wall s','Frames','Valid'))
    for r in results:
        frames = r.get('output_video_samples', r.get('ffmpeg_frames', '?'))
        print('%-9s %-8s %-12s %8s %9s %8.1f %9s %6s' %
              (r['resolution'],r['implementation'],r['backend'],
               str(r['cpu_avg'])+'%' if r['cpu_avg'] is not None else 'n/a',
               str(r['rss_avg_mib'])+' MiB' if r['rss_avg_mib'] is not None else 'n/a',
               r['elapsed_s'],frames,'YES' if r['valid'] else 'NO'))
        if r.get('fps_gate_drop', 0):
            print('  NOTE: Native FPS gate dropped %d frames before cadence detection'
                  % r['fps_gate_drop'])
    print('Result JSON:',dest)
    print('Native output counts are MPEG-TS access units; FFmpeg frame= is POST fps=25 filter.')
    print('Compare source decoded frames and Native decoded_video_frames to determine true losses.')
    print('These are encode+software decode+scale pipeline tests, not identical quality/filters.')
    print('Source TS is deleted after test; logs and summary remain in /tmp.')


if __name__ == '__main__':
    main()
