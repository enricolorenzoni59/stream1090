import json,subprocess,time,sys,hashlib
from pathlib import Path
sys.path.insert(0,str(Path.cwd()))
from dual_mine import parse_avr
from experiment import dedup
from evaluate_mixed_modes import score
from short_validation import canonical
from same_capture_registration import register_same_capture
from verify_dual import register
R=Path('runs/stream1090-precision')
rows=[]
for suite in ('fresh',):
 for cap in sorted((R/suite).glob('*/capture.cu8')):
  truth=json.loads(cap.with_name('truth.json').read_text()); label=suite+'_'+cap.parent.name
  row={'case':label,'variants':{}}
  offset=None
  for variant in ('baseline','bounded'):
   out=R/(label+'_'+variant+'.avr'); binary=R/variant
   start=time.monotonic()
   if not out.exists():
    with cap.open('rb') as fi,out.open('w') as fo,out.with_suffix('.log').open('w') as fe:
     subprocess.run([str(binary),'-s','2.4','-u','12','-q'],stdin=fi,stdout=fo,stderr=fe,check=True)
   duration=time.monotonic()-start
   ts,_,hs=parse_avr(out); ev=dedup(canonical([[float(t/5),h] for t,h in zip(ts,hs)]))
   if ev and truth:
    try:a,b,reg=register(ev,canonical(truth))
    except ValueError:a,b,reg=register_same_capture(ev,canonical(truth))
    ev=[[t*a+b,h] for t,h in ev]
   else:reg={'method':'empty truth or output; no alignment needed'}
   row['variants'][variant]={'metrics':score(ev,truth),'seconds':duration,'registration':reg}
  rows.append(row);(R/'fresh-results.json').write_text(json.dumps(rows,indent=2))
  print(label,{v:(x['metrics']['true'],x['metrics']['false']) for v,x in row['variants'].items()},flush=True)
