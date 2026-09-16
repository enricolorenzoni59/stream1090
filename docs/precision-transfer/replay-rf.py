import sys,json,subprocess,time
from pathlib import Path
sys.path.insert(0,str(Path.cwd()))
from dual_mine import parse_avr
from experiment import dedup,matched
from short_validation import canonical
from same_capture_registration import register_same_capture
R=Path('runs/stream1090-precision'); results=[]
for radio in ('A','B'):
 cap=Path('data/overnight/N42/0530UTC')/(radio+'_trim.cu8');events={};row={'radio':radio}
 for v in ('baseline','bounded'):
  out=R/('RF_'+radio+'_'+v+'.avr');start=time.monotonic()
  with cap.open('rb') as fi,out.open('w') as fo,out.with_suffix('.log').open('w') as fe:subprocess.run([str(R/v),'-s','2.4','-u','12','-q'],stdin=fi,stdout=fo,stderr=fe,check=True)
  ts,_,hs=parse_avr(out);events[v]=dedup(canonical([[float(t/5),h] for t,h in zip(ts,hs)]));row[v]={'count':len(events[v]),'seconds':time.monotonic()-start,'per_df':{str(df):sum(int(h[:2],16)>>3==df for t,h in events[v]) for df in (0,4,5,11,16,17,18,20,21)}}
 common,only,missing=matched(events['bounded'],events['baseline']);row.update(common=len(common),bounded_only=len(only),baseline_only=len(missing));results.append(row);(R/'rf-results.json').write_text(json.dumps(results,indent=2));print(row,flush=True)
