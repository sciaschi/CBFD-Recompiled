#!/usr/bin/env python3
"""
Pairs the voice clips the mod logged (voice_segments.txt) with the game's speech-bubble text
(bubble_text/, from extract_bubble_text.py) and writes the mod's clips[] transcript table, with only
Python's standard library:

  python match_voice_to_text.py VOICE_SEGMENTS [--bubbles FOLDER] [--out FILE] [--report FILE]

VOICE_SEGMENTS is the voice_segments.txt the mod writes to the game's data folder with "Dev: Capture
Voice Clips to File" or "Dev: Auto Play All Dialogues" on. FOLDER (bubble_text by default) is where
extract_bubble_text.py wrote the speech-bubble text. FILE (clips_generated.c) gets a C fragment for the
mod's clips[] table; the report (dialog_report.txt) lists every dialogue the run tried and how it went.

How the pairing works
---------------------
Each clip line names the scene and the cutscene script that was playing when the clip started, so
clips are grouped by (scene, script) — one dialogue — and in play order within it. bubble_text/ has
the same dialogue as its [script.text] blocks, each block's lines being its bubbles in order. Where a
dialogue's clip count equals its bubble count, clip N is bubble N: each is emitted as a single-line
entry ({ 0, WHOLE_CLIP, "the bubble's text" }), shown for as long as the clip plays. Where the counts
differ (a clip that speaks several bubbles, a bubble with no voice, a dialogue cut short when it
stalled on a bubble), the dialogue is listed commented out, clips beside bubbles, to pair by hand.

Older logs, without script=, are grouped by scene only (as before). The text is always the game's own,
from your ROM: nothing is guessed from audio.
"""
import argparse
import re
import sys
from pathlib import Path

CLIP_RE = re.compile(
    r'scene=([0-9A-Fa-f]+)\s+(?:script=(\w+)\s+)?rom=0x([0-9A-Fa-f]+)\s+order=(\d+)\s+'
    r'duration_ms=(\d+)\s+serial=(\d+)(?:\s+at_tick=(-?\d+))?')
DIALOG_RE = re.compile(r'dialog=(\w+)(.*)')
FIELD_RE = re.compile(r'(\w+)=([0-9A-Fa-fx-]+)')


# How good a capture of a dialogue is: a run that played to its own end beats one that stalled or hit
# the cap (its clips up to there are still real), which beats one skipped (by hand or otherwise).
OUTCOME_RANK = {'end': 4, 'capture': 3, 'stalled': 2, 'cap': 2, 'skipped': 1, 'nostart': 0, 'unfinished': 0}


def parse_log(path):
    """voice_segments.txt -> (clips by (scene, script), outcome by (scene, script)).

    A dialogue can be played in several runs (several sittings, or a re-run after a crash). Each
    dialog=start opens a new run of it; the clips tagged with that (scene, script) go into its latest
    run, and its dialog=<outcome> line closes it. The best run is kept: one that played to its end over
    one that stalled, over one that was skipped; between equals, the one with more clips, then the later
    one. So a dialogue skipped by hand in a later sitting never replaces a full capture. Clips logged
    outside auto-play (Capture alone, or an older log) form a run of their own, ranked "capture".
    script is None for clips outside any cutscene (or from an older log).
    """
    runs = {}  # key -> [ {'clips': {order: clip}, 'outcome': name, 'fields': {}, 'seq': n} ]
    seq = 0
    for raw in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        seq += 1
        m = CLIP_RE.search(line)
        if m:
            scene = int(m.group(1), 16)
            script = None if m.group(2) in (None, 'none') else int(m.group(2))
            clip = {
                'rom': int(m.group(3), 16),
                'order': int(m.group(4)),
                'duration_ms': int(m.group(5)),
                'serial': int(m.group(6)),
                'at_tick': int(m.group(7)) if m.group(7) is not None else None,
            }
            key = (scene, script)
            key_runs = runs.setdefault(key, [])
            if not key_runs or key_runs[-1]['outcome'] not in ('unfinished', 'capture'):
                # A clip with no open run (Capture alone, or after the run closed): a capture run.
                if not key_runs or key_runs[-1]['outcome'] != 'capture':
                    key_runs.append({'clips': {}, 'outcome': 'capture', 'fields': {}, 'seq': seq})
            key_runs[-1]['clips'][clip['order']] = clip
            continue
        m = DIALOG_RE.match(line)
        if m:
            fields = dict(FIELD_RE.findall(m.group(2)))
            if 'scene' not in fields or 'script' not in fields:
                continue
            key = (int(fields['scene'], 16), int(fields['script']))
            key_runs = runs.setdefault(key, [])
            if m.group(1) == 'start':
                key_runs.append({'clips': {}, 'outcome': 'unfinished', 'fields': {}, 'start': fields, 'seq': seq})
            else:
                if not key_runs or key_runs[-1]['outcome'] != 'unfinished':
                    key_runs.append({'clips': {}, 'outcome': 'unfinished', 'fields': {}, 'seq': seq})
                key_runs[-1]['outcome'] = m.group(1)
                key_runs[-1]['fields'] = fields

    clips, outcomes = {}, {}
    for key, key_runs in runs.items():
        best = max(key_runs, key=lambda r: (OUTCOME_RANK.get(r['outcome'], 0), len(r['clips']), r['seq']))
        if best['clips']:
            clips[key] = [best['clips'][o] for o in sorted(best['clips'])]
        if any(r['outcome'] != 'capture' for r in key_runs):
            outcomes[key] = {'outcome': best['outcome'], 'fields': best['fields'],
                             'start': best.get('start', {}), 'runs': len(key_runs)}
    return clips, outcomes


SCENE_HEADING_RE = re.compile(r'^Scene\s+(\d+):?')
BLOCK_RE = re.compile(r'^\[(\d+)\.(\d+)\]$')


def parse_bubbles(folder):
    """bubble_text/ -> ({(scene, script): [bubble, ...]}, {scene: name}) in play order."""
    by_dialog = {}
    names = {}
    for file in sorted(Path(folder).glob('scene_*.txt')):
        lines = file.read_text(encoding='utf-8', errors='replace').splitlines()
        m = SCENE_HEADING_RE.match(lines[0]) if lines else None
        if not m:
            continue
        scene = int(m.group(1))
        names[scene] = lines[0].split(':', 1)[1].strip() if ':' in lines[0] else ''
        script = None
        for line in lines[1:]:
            s = line.strip()
            b = BLOCK_RE.match(s)
            if b:
                script = int(b.group(1))
                by_dialog.setdefault((scene, script), [])
            elif s and script is not None:
                by_dialog[(scene, script)].append(s)
    return by_dialog, names


def c_escape(text):
    return text.replace('\\', '\\\\').replace('"', '\\"')


def emit(clips, bubbles, events, names):
    out = [
        '// Generated by match_voice_to_text.py: paste the matched entries into clips[].',
        '// Each matched clip is one voiced bubble, shown for the whole clip.',
        '// Dialogues whose clip and bubble counts differ are commented out, to pair by hand.',
        '',
    ]
    matched = ambiguous = matched_clips = 0
    for key in sorted(clips, key=lambda k: (k[0], -1 if k[1] is None else k[1])):
        scene, script = key
        cl = clips[key]
        bl = bubbles.get(key, []) if script is not None else []
        label = 'scene 0x%02X %s' % (scene, 'script %d' % script if script is not None else '(no cutscene)')
        title = names.get(scene, '')
        exact = script is not None and len(cl) == len(bl) and cl
        out.append('// %s%s: %d clip(s), %d bubble(s)%s' % (
            label, ' (%s)' % title if title else '', len(cl), len(bl), '' if exact else '  <-- check by hand'))
        if exact:
            matched += 1
            matched_clips += len(cl)
            for clip, bubble in zip(cl, bl):
                out.append('static const SubLine clip_%02X_%d_%d[] = { { 0, WHOLE_CLIP, "%s" } };' % (
                    scene, script, clip['order'], c_escape(bubble)))
            out.append('// clips[] rows:')
            for clip in cl:
                out.append('//   { 0x%08X, 0, LINES(clip_%02X_%d_%d) },  // %u ms%s' % (
                    clip['rom'], scene, script, clip['order'], clip['duration_ms'],
                    ', at tick %d' % clip['at_tick'] if clip['at_tick'] is not None else ''))
        else:
            ambiguous += 1
            out.append('// clips (order: ROM, length, cutscene tick at start):')
            for clip in cl:
                out.append('//   %d: 0x%08X  %u ms%s' % (
                    clip['order'], clip['rom'], clip['duration_ms'],
                    '  tick %d' % clip['at_tick'] if clip['at_tick'] is not None else ''))
            if bl:
                out.append('// bubbles:')
                for i, bubble in enumerate(bl):
                    out.append('//   [%d] %s' % (i, bubble))
            ev = events.get(key)
            if ev and ev['outcome'] in ('stalled', 'cap', 'skipped'):
                out.append('// best capture was %s (ticks=%s): it may be missing clips after that point' % (
                    ev['outcome'], ev['fields'].get('ticks', '?')))
        out.append('')
    return '\n'.join(out) + '\n', (matched, matched_clips, ambiguous)


def report(events, clips):
    """A plain-text summary of every dialogue the auto-play runs tried, with its best capture."""
    lines = ['# dialog_report.txt - each dialogue auto-play tried, with its best capture', '']
    counts = {}
    for key in sorted(events):
        ev = events[key]
        counts[ev['outcome']] = counts.get(ev['outcome'], 0) + 1
        lines.append('scene 0x%02X script %-3d text=%s  %-10s clips=%-3d runs=%d %s' % (
            key[0], key[1], ev['start'].get('text', '?'), ev['outcome'], len(clips.get(key, [])), ev['runs'],
            ' '.join('%s=%s' % kv for kv in ev['fields'].items() if kv[0] not in ('scene', 'script', 'clips'))))
    lines.insert(1, 'outcomes: ' + ', '.join('%s %d' % kv for kv in sorted(counts.items())))
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description="Pairs logged voice clips with the game's speech-bubble text.")
    parser.add_argument('voice_segments', help='voice_segments.txt from the mod')
    parser.add_argument('--bubbles', default='bubble_text', help='extract_bubble_text.py output (default: bubble_text)')
    parser.add_argument('--out', default='clips_generated.c', help='C fragment to write (default: clips_generated.c)')
    parser.add_argument('--report', default='dialog_report.txt', help='run report to write (default: dialog_report.txt)')
    args = parser.parse_args()

    if not Path(args.voice_segments).is_file():
        sys.exit('voice segments file not found: %s' % args.voice_segments)
    if not Path(args.bubbles).is_dir():
        sys.exit('bubble text folder not found: %s (run extract_bubble_text.py first)' % args.bubbles)

    clips, events = parse_log(args.voice_segments)
    if not clips and not events:
        sys.exit('nothing logged in %s (was Capture or Auto Play on?)' % args.voice_segments)
    bubbles, names = parse_bubbles(args.bubbles)
    fragment, (matched, matched_clips, ambiguous) = emit(clips, bubbles, events, names)
    Path(args.out).write_text(fragment, encoding='utf-8')
    print('%d dialogue(s) matched exactly (%d clips); %d to check by hand. Wrote %s' % (
        matched, matched_clips, ambiguous, args.out))
    if events:
        Path(args.report).write_text(report(events, clips), encoding='utf-8')
        print('Run report: %s' % args.report)


if __name__ == '__main__':
    main()
