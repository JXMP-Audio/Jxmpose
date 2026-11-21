#!/usr/bin/env python
# Copyright (C) 2026 JXMP Audio
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Port lists of the JX plugins; the wscript writes manifest.ttl and one
# <plugin>.ttl per plugin from these. Port order must match the enums in
# the C sources.

URI = 'urn:jxmp:'

PREFIXES = """@prefix atom:  <http://lv2plug.in/ns/ext/atom#> .
@prefix doap:  <http://usefulinc.com/ns/doap#> .
@prefix foaf:  <http://xmlns.com/foaf/0.1/> .
@prefix idpy:  <http://harrisonconsoles.com/lv2/inlinedisplay#> .
@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .
@prefix midi:  <http://lv2plug.in/ns/ext/midi#> .
@prefix patch: <http://lv2plug.in/ns/ext/patch#> .
@prefix pprop: <http://lv2plug.in/ns/ext/port-props#> .
@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .
@prefix state: <http://lv2plug.in/ns/ext/state#> .
@prefix units: <http://lv2plug.in/ns/extensions/units#> .
@prefix urid:  <http://lv2plug.in/ns/ext/urid#> .
@prefix work:  <http://lv2plug.in/ns/ext/worker#> .
"""

# ---- port constructors ----

def ain(sym, name, sidechain=False):
    return dict(kind='ain', sym=sym, name=name, sidechain=sidechain)

def aout(sym, name):
    return dict(kind='aout', sym=sym, name=name)

def ctl(sym, name, default, mn, mx, unit=None, log=False, integer=False, enum=None, toggle=False, out=False, latency=False, notauto=False):
    return dict(kind='cout' if out else 'cin', sym=sym, name=name, default=default, min=mn, max=mx,
                unit=unit, log=log, integer=integer, enum=enum, toggle=toggle, latency=latency, notauto=notauto)

def toggle(sym, name, default=0):
    return ctl(sym, name, default, 0, 1, toggle=True)

def enum(sym, name, default, labels):
    return ctl(sym, name, default, 0, len(labels) - 1, integer=True, enum=labels)

def meter(sym, name, mn, mx, unit=None):
    return ctl(sym, name, mn, mn, mx, unit=unit, out=True)

def midi_in(sym='midi_in', name='MIDI In', patch=False):
    return dict(kind='min', sym=sym, name=name, patch=patch)

def atom_out(sym='notify', name='Notify'):
    return dict(kind='aout_atom', sym=sym, name=name)

def stereo_io():
    return [ain('in_l', 'In L'), ain('in_r', 'In R'), aout('out_l', 'Out L'), aout('out_r', 'Out R')]

NOTES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
SCALES = ['Chromatic', 'Major', 'Minor', 'Dorian', 'Phrygian', 'Lydian', 'Mixolydian', 'Locrian',
          'Harmonic Minor', 'Major Pentatonic', 'Minor Pentatonic', 'Arabian', 'Egyptian']

# ---- the plugins ----

def mbc():
    ports = stereo_io() + [
        ctl('xover1', 'Crossover Low', 120, 20, 1000, 'hz', log=True),
        ctl('xover2', 'Crossover Mid', 1000, 200, 8000, 'hz', log=True),
        ctl('xover3', 'Crossover High', 6000, 1000, 18000, 'hz', log=True),
    ]
    for b in range(1, 5):
        ports += [
            ctl('thr%d' % b, 'Band %d Threshold' % b, -18, -60, 0, 'db'),
            ctl('ratio%d' % b, 'Band %d Ratio' % b, 3, 1, 20, log=True),
            ctl('att%d' % b, 'Band %d Attack' % b, 10, 0.1, 200, 'ms', log=True),
            ctl('rel%d' % b, 'Band %d Release' % b, 150, 5, 2000, 'ms', log=True),
            ctl('makeup%d' % b, 'Band %d Makeup' % b, 0, 0, 24, 'db'),
            toggle('active%d' % b, 'Band %d Active' % b, 1),
            toggle('solo%d' % b, 'Band %d Solo' % b, 0),
        ]
    ports += [
        ctl('knee', 'Knee', 6, 0, 18, 'db'),
        ctl('gain', 'Output', 0, -24, 24, 'db'),
    ]
    ports += [meter('gr%d' % b, 'Band %d Reduction' % b, 0, 40, 'db') for b in range(1, 5)]
    return dict(sym='jx-mbc', name='JX-MBC Multiband Compressor', cls='CompressorPlugin',
                comment='Four band compressor with Linkwitz-Riley crossovers (bands sum flat), soft knee, solo per band.',
                ports=ports)

def voc():
    ports = [ain('in_l', 'Voice L'), ain('in_r', 'Voice R'),
             ain('sc_l', 'Carrier L', sidechain=True), ain('sc_r', 'Carrier R', sidechain=True),
             aout('out_l', 'Out L'), aout('out_r', 'Out R'),
             ctl('bands', 'Bands', 16, 4, 32, integer=True),
             enum('source', 'Carrier', 0, ['Sidechain', 'Noise']),
             ctl('wet', 'Wet', 0, -60, 12, 'db'),
             ctl('dry', 'Voice Dry', -60, -60, 12, 'db'),
             ctl('cdry', 'Carrier Dry', -60, -60, 12, 'db'),
             toggle('invert', 'Swap Inputs'),
             ctl('attack', 'Attack', 5, 0.5, 100, 'ms', log=True),
             ctl('release', 'Release', 40, 5, 500, 'ms', log=True),
             ctl('shift', 'Formant Shift', 0, -12, 12, 'semitone12TET'),
             ctl('sibilance', 'Sibilance', -60, -60, 0, 'db')]
    return dict(sym='jx-voc', name='JX-VOC Vocoder', cls='SpectralPlugin',
                comment='Channel vocoder: the voice on the main input shapes the carrier on the sidechain (or internal noise). 4 to 32 bands.',
                ports=ports)

def pit():
    ports = stereo_io() + [
        ctl('window', 'Window', 60, 20, 120, 'ms'),
        ctl('wet', 'Wet', 0, -60, 12, 'db'),
        ctl('dry', 'Dry', -60, -60, 12, 'db')]
    for s, on, semi in (('A', 1, 12), ('B', 0, 7)):
        ports += [
            toggle('on_%s' % s.lower(), 'Shifter %s' % s, on),
            ctl('semi_%s' % s.lower(), 'Shifter %s Semitones' % s, semi, -24, 24, 'semitone12TET', integer=True),
            ctl('cent_%s' % s.lower(), 'Shifter %s Cents' % s, 0, -100, 100, 'cent'),
            ctl('level_%s' % s.lower(), 'Shifter %s Level' % s, 0, -60, 12, 'db'),
            ctl('pan_%s' % s.lower(), 'Shifter %s Pan' % s, 0, -1, 1)]
    # appended: older sessions keep their port indices
    ports += [toggle('formant', 'Keep Formants'),
              ctl('latency', 'Latency', 0, 0, 8192, out=True, latency=True)]
    return dict(sym='jx-pit', name='JX-PIT Pitch Shifter', cls='PitchPlugin',
                comment='Two pitch shifters (semitones and cents, level and pan each) mixed with the dry signal. No latency; Keep Formants (for a single voice) keeps the timbre of the voice at 15 ms latency.',
                ports=ports)

def tun():
    ports = stereo_io() + [
        enum('mode', 'Mode', 0, ['Tuner', 'Correct']),
        ctl('ref', 'Reference A', 440, 400, 480, 'hz'),
        enum('key', 'Key', 0, NOTES),
        enum('scale', 'Scale', 0, SCALES),
        ctl('speed', 'Correction Speed', 20, 0, 500, 'ms'),
        ctl('amount', 'Correction Amount', 100, 0, 100, 'pc'),
        toggle('mute', 'Mute While Tuning'),
        meter('freq', 'Frequency', 0, 2000, 'hz'),
        meter('note', 'Note', 0, 127),
        meter('cents', 'Cents', -50, 50, 'cent'),
        # appended: older sessions keep their port indices
        toggle('formant', 'Keep Formants', 1),
        ctl('latency', 'Latency', 0, 0, 8192, out=True, latency=True)]
    return dict(sym='jx-tun', name='JX-TUN Tuner', cls='PitchPlugin',
                comment='Tuner (note, cents, frequency) and automatic pitch correction to the nearest note of a key and scale. Keep Formants (on by default) keeps the timbre of the voice at 15 ms latency in Correct mode.',
                ports=ports)

def dnr():
    ports = stereo_io() + [
        toggle('learn', 'Learn Noise'),
        ctl('reduction', 'Reduction', 24, 0, 60, 'db'),
        ctl('threshold', 'Threshold', 6, -12, 24, 'db'),
        ctl('smooth', 'Smoothing', 50, 0, 100, 'pc'),
        ctl('gain', 'Output', 0, -24, 12, 'db'),
        toggle('listen', 'Listen to Removed'),
        ctl('latency', 'Latency', 0, 0, 8192, out=True, latency=True)]
    return dict(sym='jx-dnr', name='JX-DNR Noise Reduction', cls='SpectralPlugin',
                comment='Spectral noise reduction: learn a noise profile from a passage of noise only, then subtract it.',
                required=['urid:map'], extension=['state:interface'], ports=ports)

def frz():
    ports = stereo_io() + [
        toggle('hold', 'Hold'),
        toggle('recapture', 'Recapture'),
        ctl('level', 'Hold Level', 0, -60, 12, 'db'),
        ctl('dry_hold', 'Dry While Holding', -60, -60, 12, 'db'),
        ctl('dry', 'Dry', 0, -60, 12, 'db'),
        ctl('drift', 'Phase Drift', 20, 0, 100, 'pc'),
        ctl('fade', 'Fade', 150, 10, 5000, 'ms', log=True)]
    return dict(sym='jx-frz', name='JX-FRZ Spectral Freeze', cls='SpectralPlugin',
                comment='Hold freezes the spectrum of what plays and sustains it as a drone until released; Recapture takes a new snapshot.',
                ports=ports)

def hrm():
    ports = [midi_in()] + stereo_io() + [
        ctl('wet', 'Wet', 0, -60, 12, 'db'),
        ctl('dry', 'Dry', 0, -60, 12, 'db'),
        ctl('gain', 'Input Gain', 0, -24, 24, 'db'),
        ctl('voices', 'Max Voices', 4, 1, 8, integer=True),
        ctl('attack', 'Attack', 20, 1, 500, 'ms', log=True),
        ctl('release', 'Release', 200, 5, 2000, 'ms', log=True),
        ctl('channel', 'MIDI Channel', 0, 0, 16, integer=True),
        ctl('center', 'Center Note', 60, 24, 96, 'midiNote', integer=True),
        ctl('window', 'Window', 40, 20, 100, 'ms'),
        ctl('spread', 'Stereo Spread', 50, 0, 100, 'pc'),
        ctl('glide', 'Glide', 0, 0, 500, 'ms'),
        # appended: older sessions keep their port indices
        toggle('formant', 'Keep Formants'),
        ctl('latency', 'Latency', 0, 0, 8192, out=True, latency=True)]
    return dict(sym='jx-hrm', name='JX-HRM Harmonizer', cls='PitchPlugin',
                comment='MIDI played harmonizer: every held note adds the input shifted by (note - Center Note) semitones. Up to 8 voices. MIDI Channel 0 = all.',
                required=['urid:map'], ports=ports)

def spa():
    ports = stereo_io() + [
        enum('size', 'FFT Size', 2, ['1024', '2048', '4096', '8192']),
        enum('window', 'Window', 0, ['Blackman-Harris', 'Blackman', 'Hann', 'Rectangular']),
        ctl('integrate', 'Integrate', 100, 0, 5000, 'ms'),
        ctl('floor', 'Floor', -96, -160, -30, 'db'),
        toggle('peak', 'Peak Hold', 1),
        ctl('slope', 'Slope', 0, -6, 6),
        enum('channel', 'Channel', 0, ['L + R', 'Left', 'Right', 'L - R'])]
    return dict(sym='jx-spa', name='JX-SPA Spectrum Analyzer', cls='AnalyserPlugin',
                comment='Spectrum analyzer: FFT size, window, integration, floor, slope (dB per octave around 1 kHz) and peak hold. Audio passes unchanged.',
                ports=ports)

def loop():
    ports = stereo_io() + [
        enum('monitor', 'Input Monitor', 1, ['Auto', 'On', 'Off']),
        ctl('level', 'Loops Level', 0, -60, 12, 'db')]
    for s in range(1, 9):
        ports += [
            toggle('rec%d' % s, 'Slot %d Rec' % s),
            toggle('play%d' % s, 'Slot %d Play' % s, 1),
            toggle('rev%d' % s, 'Slot %d Reverse' % s),
            ctl('level%d' % s, 'Slot %d Level' % s, 0, -60, 12, 'db'),
            toggle('clear%d' % s, 'Slot %d Clear' % s)]
    ports += [meter('length', 'Loop Length', 0, 30, 's'),
              meter('pos', 'Position', 0, 1)]
    return dict(sym='jx-loop', name='JX-LOOP Looper', cls='UtilityPlugin',
                comment='Eight synchronised loop slots. The first recording sets the loop length; later ones record in sync and overdub. Up to 30 s.',
                ports=ports)

def smp():
    ports = [midi_in('control', 'MIDI In', patch=True), atom_out('notify', 'Notify'),
             aout('out_l', 'Out L'), aout('out_r', 'Out R'),
             ctl('volume', 'Volume', 0, -60, 12, 'db'),
             ctl('min_volume', 'Min Volume', -24, -60, 0, 'db'),
             ctl('pan', 'Pan', 0, -1, 1),
             enum('mode', 'Mode', 0, ['Note (transposed)', 'Sample (fixed pitch)']),
             ctl('note_lo', 'Note Start', 0, 0, 127, 'midiNote', integer=True),
             ctl('note_hi', 'Note End', 127, 0, 127, 'midiNote', integer=True),
             ctl('root', 'Root Note', 60, 0, 127, 'midiNote', integer=True),
             ctl('tune', 'Pitch Offset', 0, -24, 24, 'semitone12TET'),
             ctl('bend', 'Pitch Bend Range', 2, 0, 24, 'semitone12TET'),
             ctl('attack', 'Attack', 1, 0, 2000, 'ms'),
             ctl('decay', 'Decay', 500, 1, 10000, 'ms', log=True),
             ctl('sustain', 'Sustain', 0, -60, 0, 'db'),
             ctl('release', 'Release', 100, 1, 10000, 'ms', log=True),
             toggle('obey_off', 'Obey Note-Offs', 1),
             toggle('loop', 'Loop'),
             ctl('loop_start', 'Loop Start', 0, 0, 99, 'pc'),
             ctl('start', 'Start Offset', 0, 0, 99, 'pc'),
             ctl('voices', 'Max Voices', 16, 1, 32, integer=True),
             ctl('probability', 'Probability', 100, 0, 100, 'pc'),
             ctl('channel', 'MIDI Channel', 0, 0, 16, integer=True)]
    return dict(sym='jx-smp', name='JX-SMP Sampler', cls='InstrumentPlugin',
                comment='Plays one sample from MIDI: key range, root note, velocity to level, ADSR, loop, start offset, voice limit, trigger probability.',
                required=['urid:map', 'work:schedule'], extension=['work:interface', 'state:interface'],
                parameters=[('jx-smp#sample', 'Sample', 'Path')], ports=ports)

PLUGINS = [mbc, voc, pit, tun, dnr, frz, hrm, spa, loop, smp]

# ---- writer ----

def _num(v):
    return repr(float(v))

def _port(i, p):
    l = []
    k = p['kind']
    if k == 'ain':
        l.append('a lv2:InputPort, lv2:AudioPort')
    elif k == 'aout':
        l.append('a lv2:OutputPort, lv2:AudioPort')
    elif k == 'min':
        l.append('a lv2:InputPort, atom:AtomPort')
        l.append('atom:bufferType atom:Sequence')
        l.append('atom:supports midi:MidiEvent' + (', patch:Message' if p['patch'] else ''))
        l.append('lv2:designation lv2:control')
    elif k == 'aout_atom':
        l.append('a lv2:OutputPort, atom:AtomPort')
        l.append('atom:bufferType atom:Sequence')
        l.append('atom:supports patch:Message')
        l.append('lv2:designation lv2:control')
    elif k == 'cin':
        l.append('a lv2:InputPort, lv2:ControlPort')
    else:
        l.append('a lv2:OutputPort, lv2:ControlPort')
    l.append('lv2:index %d' % i)
    l.append('lv2:symbol "%s"' % p['sym'])
    l.append('lv2:name "%s"' % p['name'])
    if k in ('cin', 'cout'):
        l.append('lv2:default %s' % _num(p['default']))
        l.append('lv2:minimum %s' % _num(p['min']))
        l.append('lv2:maximum %s' % _num(p['max']))
        props = []
        if p['log']:
            props.append('pprop:logarithmic')
        if p['integer'] or p['enum']:
            props.append('lv2:integer')
        if p['enum']:
            props.append('lv2:enumeration')
        if p['toggle']:
            props.append('lv2:toggled')
        if p['latency']:
            props.append('lv2:reportsLatency')
            l.append('lv2:designation lv2:latency')
        if p['notauto']:
            props.append('pprop:notAutomatic')
        if props:
            l.append('lv2:portProperty ' + ', '.join(props))
        if p['unit']:
            l.append('units:unit units:%s' % p['unit'])
        if p['enum']:
            sp = ['[ rdfs:label "%s" ; rdf:value %d ]' % (lab, v) for v, lab in enumerate(p['enum'])]
            l.append('lv2:scalePoint ' + ' ,\n        '.join(sp))
    if k == 'ain' and p.get('sidechain'):
        l.append('lv2:portProperty lv2:isSideChain')
    return '    [\n        ' + ' ;\n        '.join(l) + ' ;\n    ]'

def plugin_ttl(spec):
    uri = URI + spec['sym']
    out = [PREFIXES.replace('@prefix rdfs:', '@prefix rdf:   <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .\n@prefix rdfs:')]
    out.append("""<https://github.com/JXMP-Audio>
    a foaf:Person ;
    foaf:name "JXMP Audio" .

idpy:queue_draw a lv2:Feature .
idpy:in_gui a lv2:Feature .
idpy:interface a lv2:ExtensionData .
""")
    for par in spec.get('parameters', []):
        out.append('<%s>\n    a lv2:Parameter ;\n    rdfs:label "%s" ;\n    rdfs:range atom:%s .\n' % (URI + par[0], par[1], par[2]))

    head = ['a lv2:Plugin, doap:Project, lv2:%s' % spec['cls'],
            'doap:name "%s"' % spec['name'],
            'rdfs:comment "%s"' % spec['comment'],
            'doap:maintainer <https://github.com/JXMP-Audio>',
            'doap:license <http://usefulinc.com/doap/licenses/gpl>',
            'lv2:minorVersion 1',
            'lv2:microVersion 0']
    req = spec.get('required', [])
    if req:
        head.append('lv2:requiredFeature ' + ', '.join(req))
    head.append('lv2:optionalFeature ' + ', '.join(['lv2:hardRTCapable', 'idpy:queue_draw', 'idpy:in_gui'] + spec.get('optional', [])))
    head.append('lv2:extensionData ' + ', '.join(['idpy:interface'] + spec.get('extension', [])))
    for par in spec.get('parameters', []):
        head.append('patch:writable <%s>' % (URI + par[0]))
    ports = ' ,\n'.join(_port(i, p) for i, p in enumerate(spec['ports']))
    out.append('<%s>\n    %s ;\n\n    lv2:port\n%s .\n' % (uri, ' ;\n    '.join(head), ports))
    return '\n'.join(out)

def manifest_ttl(specs, lib_ext):
    out = ['@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .', '@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .', '']
    for s in specs:
        out.append('<%s%s>\n    a lv2:Plugin ;\n    lv2:binary <jxmp%s> ;\n    rdfs:seeAlso <%s.ttl> .\n' % (URI, s['sym'], lib_ext, s['sym']))
    return '\n'.join(out)

def specs():
    return [f() for f in PLUGINS]
