"""Verify measured-frame accounting, delayed GPU records and localized reports."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'scripts/performance'))
from analysis_views import frame_breakdown, native_view_timings
from summarize_trace import statistics_of
from trace_report import write_reports


def event(name,first,stop,thread='main'):
    return {'name':name,'src_file':'unit.cpp','src_line':'1','thread':thread,
            'ns_since_start':str(first),'exec_time_ns':str(stop-first)}


def view(kind,identity,first,stop,frame=10,name='Opaque',timestamp=1):
    data={'viewId':identity,'name':name,'group':4,'sourceFrame':frame,
          'begin':first,'end':stop,'frequency':1000}
    return {'total_ns':str(timestamp),'MessageName':'Render.'+kind+' '+json.dumps(data)}


class AnalysisTest(unittest.TestCase):
    def test_nested_frames_and_parallel_threads_do_not_double_count(self):
        events=[]
        own=[]
        for origin in [0,10_000_000]:
            for name,a,b,self_ns in [('Host.frame',0,10,0),('Stage',0,6,4),
                                     ('Leaf',1,3,2),('Wait',6,10,4)]:
                row=event(name,origin+a*1_000_000,origin+b*1_000_000)
                events.append(row)
                own.append({**row,'exec_time_ns':str(self_ns*1_000_000)})
        worker=event('Worker',5_000_000,15_000_000,'worker')
        events.append(worker);own.append(worker)
        result=frame_breakdown(events,own,'Host.frame',20_000_000)
        self.assertEqual(result['frames'],2)
        self.assertEqual(result['direct_stage_coverage_percent'],100)
        root=result['roots'][0]
        self.assertEqual(root['per_frame_ms'],10)
        stage=next(row for row in root['children'] if row['name']=='Stage')
        self.assertEqual(stage['per_frame_ms'],6)
        self.assertEqual(stage['self_per_frame_ms'],4)
        self.assertEqual(stage['calls'],2)
        self.assertEqual(sum(row['per_frame_ms'] for row in root['children']),10)
        self.assertEqual(next(row for row in result['threads'] if row['thread']=='worker')['covered_window_percent'],50)
        self.assertEqual(next(row for row in result['threads'] if row['thread']=='main')['wait_related_ms'],8)

    def test_crossed_cpu_scopes_are_rejected(self):
        events=[event('Host.frame',0,20),event('A',1,10),event('B',9,15)]
        with self.assertRaises(ValueError):
            frame_breakdown(events,events,'Host.frame',20)

    def test_missing_self_time_is_not_replaced_by_inclusive_time(self):
        events=[event('Host.frame',0,20),event('Stage',1,10)]
        with self.assertRaises(ValueError):
            frame_breakdown(events,events[:1],'Host.frame',20)

    def test_delayed_gpu_frames_are_retained_and_duplicate_polls_are_removed(self):
        first=view('gpuView',80,100,110)
        messages=[first,first,view('gpuView',113,105,115),
                  view('gpuView',80,50,55,frame=9),view('cpuView',130,1,4,frame=-1)]
        result=native_view_timings(messages,0,20,statistics_of)
        self.assertEqual(result['raw_unique_records']['gpuView'],3)
        shadow=next(row for row in result['views'] if row['view_id']==80)
        self.assertEqual(shadow['gpu_source_frames'],[9,10])
        self.assertEqual(shadow['gpu_ms']['mean'],7.5)
        combined=next(row for row in result['gpu_view_frames'] if row['source_frame']==10)
        self.assertEqual(combined['union_ms'],15)
        self.assertEqual(combined['sum_ms'],20)
        self.assertEqual(result['missing_gpu_view_ids'],[130])
        self.assertEqual(next(row for row in result['views'] if row['view_id']==130)['gpu_ms'],{'count':0})

    def test_invalid_timestamps_and_unbalanced_callbacks_are_rejected(self):
        with self.assertRaises(ValueError):
            native_view_timings([view('gpuView',113,10,2)],0,20,statistics_of)
        with self.assertRaises(ValueError):
            native_view_timings([{'total_ns':'1','MessageName':'Bgfx.profilerStackImbalance'}],0,20,statistics_of)

    def test_report_is_localized_self_contained_and_escapes_trace_names(self):
        locale=json.loads((ROOT/'scripts/performance/locales/zh-cn.json').read_text())
        sample={'trace':'capture.tracy','trace_sha256':'0'*64,'window':{'seconds':1},
            'frame_ms':statistics_of([10,12]),'excluded_zone_events':0,'plots':{},
            'zones':[{'name':'<script>bad()</script>','source':'unit.cpp','line':1,
                'thread':'main','self_ms':statistics_of([1])}],
            'coverage':{'bgfx_callback_events':0},'cpu_frames':{'frames':2,'roots':[],'threads':[]},
            'frame_intervals':[{'duration_ms':10},{'duration_ms':12}],
            'renderer_views':{'views':[],'missing_gpu_view_ids':[]}}
        with tempfile.TemporaryDirectory() as directory:
            sources=[]
            for mode in ['off','on']:
                target=Path(directory)/mode
                (target/'analysis').mkdir(parents=True)
                source=target/'analysis/summary.json'
                source.write_text(json.dumps(sample))
                (target/'capture.json').write_text(json.dumps({
                    'metadata':{'case':'test-case','mode':mode,'backend':'Metal',
                        'actualSettings':{'native':{'render':{'vsync':False}}}},
                    'rss_summary_bytes':{'max':1048576}}))
                sources.append(source)
            report=write_reports(sources,Path(directory)/'report.html').read_text()
            labeled=write_reports(sources,Path(directory)/'labeled.html',
                                  ['Before <unsafe>','After']).read_text()
            self.assertIn('Before &lt;unsafe&gt;',labeled)
            self.assertNotIn('<unsafe>',labeled)
            with self.assertRaises(ValueError):
                write_reports(sources,Path(directory)/'invalid.html',['Only one'])
        self.assertIn('lang="zh-CN"',report)
        self.assertIn(locale['title'],report)
        self.assertIn(locale['unknown'],report)
        self.assertIn(locale['modes']['off'],report)
        self.assertIn(locale['modes']['on'],report)
        self.assertIn(locale['conditions'],report)
        self.assertIn(locale['paired_observations'],report)
        self.assertIn('&lt;script&gt;bad()',report)
        self.assertNotIn('<script>bad()',report)
        self.assertNotIn('<script src=',report)
        self.assertIn('viewBox="0 0 1000 190"',report)


if __name__=='__main__':
    unittest.main()
