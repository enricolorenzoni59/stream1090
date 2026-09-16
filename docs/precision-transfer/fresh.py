from pathlib import Path
# Freeze a new-seed validation fixture with allocated-range ICAOs. No change
# to waveform, SNR, collision, payload or scoring logic of the old generator.
s=Path('overnight_factorial_stream.py').read_text()
s=s[:s.index('\ndef main(')] if '\ndef main(' in s else s
s=s.replace('0xbcd000,0xbcd014','0xabc000,0xabc014')
ns={'__name__':'fixture_generator'};exec(compile(s,'overnight_factorial_stream.py','exec'),ns)
for seed in (9101,9103):ns['make'](Path('runs/stream1090-precision/fresh')/str(seed),seed,'near',True)
