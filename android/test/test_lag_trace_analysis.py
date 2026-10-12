"""Generated traces only: linkage, wrap, incomplete files and clock mismatch."""
import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('lag',Path(__file__).parents[1]/'tools/analyze-lag-trace.py')
lag=importlib.util.module_from_spec(spec)
spec.loader.exec_module(lag)

def capture(rows,dropped=0):
    return ('# xemu_lag schema=44 token=1 pid=100 begin_us=1000000 end_us=1300000 phase_min_us=50 capacity=262144 clock=MONOTONIC\n'
            'seq,tid,kind,start_us,end_us,frame,id,link,a,b,c\n'+
            '\n'.join(f'{i},'+r for i,r in enumerate(rows,1))+
            f'\n# complete records={len(rows)} dropped={dropped}\n')

class TraceTests(unittest.TestCase):
    def test_linked_wait(self):
        rows=['10,cb_begin,1010000,1010000,1,7,0,0,0,0',
              '10,enqueue,1020000,1020001,1,7,0,0,0,0',
              '20,worker,1020000,1025000,1,7,0,0,0,0',
              '20,submit,1025000,1026000,1,7,0,0,0,0',
              '10,slot_wait,1027000,1090000,1,8,7,0,0,0',
              '10,flip,1010000,1100000,2,2,8,0,0,0']
        r,_=lag.analyze(capture(rows))
        self.assertEqual(r['slow_frames'][0]['duration_us'],90000)
        self.assertFalse(r['wait_dependencies'][0]['missing_submit'])
        self.assertEqual(r['wait_dependencies'][0]['target'],7)
        self.assertEqual(r['wait_dependencies'][0]['chain']['worker'][0]['tid'],20)
    def test_wrap_and_unsigned(self):
        for bits in (36,48,64):
            mask=(1<<bits)-1
            self.assertEqual(lag.tick_delta(mask-2,4,bits),7)
            self.assertEqual(lag.signed_ticks(mask-2,4,bits),-7)
    def test_relative_gpu_without_calibration(self):
        r,_=lag.analyze(capture(['10,gpu_caps,1010000,1010000,1,7,0,36,1000,0',
            f'10,gpu_main,1090000,1090000,1,7,0,{(1<<36)-10},10,1000']))
        self.assertEqual(r['gpu_intervals'][0]['duration_us'],.02)
        self.assertFalse(r['gpu_intervals'][0]['aligned'])
    def test_calibration_and_contradiction(self):
        rows=['10,gpu_caps,1010000,1010000,1,7,0,64,1000,1',
              '20,submit,1020000,1020010,1,7,0,0,0,0',
              '10,calibration,1100000,1100010,1,7,0,1000000000,1100005000,100',
              '10,gpu_main,1100020,1100020,1,7,0,950000000,990000000,1000']
        r,_=lag.analyze(capture(rows))
        self.assertTrue(r['gpu_intervals'][0]['aligned'])
        self.assertEqual(r['gpu_intervals'][0]['duration_us'],40000)
        rows[-1]='10,gpu_main,1100020,1100020,1,7,0,800000000,900000000,1000'
        r,_=lag.analyze(capture(rows))
        self.assertFalse(r['gpu_intervals'][0]['aligned'])
        self.assertEqual(r['invalid_gpu_alignment_ids'],[7])
    def test_reject_incomplete_and_dropped(self):
        for text in ('',capture([],1),capture([]).rsplit('# complete',1)[0],
                     capture(['10,flip,1000020,1000010,1,1,0,0,0,0'])):
            with self.assertRaises(ValueError):lag.analyze(text)
    def test_driving_bounds(self):
        text=capture(['10,flip,1010000,1050000,1,1,0,0,0,0',
                      '10,flip,1050000,1110000,2,2,0,0,0,0'])
        r,_=lag.analyze(text,1050000,1120000)
        self.assertEqual(r['complete_frames'],1)
        self.assertEqual(r['slow_frames'][0]['frame'],2)
        with self.assertRaises(ValueError):lag.analyze(text,999999,1120000)

if __name__=='__main__':unittest.main()
