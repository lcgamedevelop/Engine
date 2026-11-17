"""Render portable localized reports from the existing official-trace summaries."""
import argparse
import html
import json
from pathlib import Path
import re


def escape(value):
    return html.escape(str(value), quote=True)


def table(headers, rows, css=""):
    return '<table class="'+css+'"><thead><tr>'+''.join('<th>'+escape(v)+'</th>' for v in headers)+\
        '</tr></thead><tbody>'+''.join('<tr>'+''.join('<td data-label="'+escape(headers[i])+\
        '">'+str(v)+'</td>' for i,v in enumerate(row))+'</tr>' for row in rows)+'</tbody></table>'


def number(value, missing):
    return f'{value:,.3f}' if isinstance(value, (int,float)) else missing


def write_reports(summaries, output):
    locale=json.loads((Path(__file__).parent/'locales/zh-cn.json').read_text())
    missing=locale['unknown']
    def label(name):
        view=re.fullmatch(r'\s*(\d+)(C?)\s+(.+)',name)
        if view:
            name=view.group(3)
            return escape(locale['native_views'].get(name,name))
        return escape(locale['zones'].get(name,name))
    def marker(name):
        return label(name)+'<code>'+escape(name)+'</code>'
    def heading(key):
        return '<h2>'+locale[key]+'</h2>'
    def value(stats,key='mean'):
        return number(stats.get(key),missing) if stats.get('count') else missing
    def sample_name(path,capture):
        metadata=capture.get('metadata',{})
        mode=metadata.get('mode','')
        return metadata.get('case',path.parent.parent.name)+' / '+locale['modes'].get(mode,mode)
    loaded=[]
    for source in summaries:
        path=Path(source).resolve()
        data=json.loads(path.read_text())
        capture_path=path.parent.parent/'capture.json'
        capture=json.loads(capture_path.read_text()) if capture_path.exists() else {}
        loaded.append((path,data,capture))
    body='<h1>'+locale['title']+'</h1><p>'+locale['subtitle']+'</p>'
    if len(loaded)>1:
        rows=[]
        for path,data,capture in loaded:
            name=sample_name(path,capture)
            rss=capture.get('rss_summary_bytes') or {}
            rows.append([escape(name),value(data['frame_ms'],'p50'),value(data['frame_ms'],'p95'),
                number(rss['max']/1048576,missing) if 'max' in rss else missing,
                value(data['plots'].get('Bgfx.drawCalls',{})),value(data['plots'].get('Bgfx.computeCalls',{}))])
        body+=table([locale['scope'],locale['p50']+' ms',locale['p95']+' ms',locale['rss_peak']+' MiB',
            locale['plots']['Bgfx.drawCalls'],locale['plots']['Bgfx.computeCalls']],rows)
        body+='<p>'+locale['comparison_note']+'</p>'
        rows=[]
        for path,data,capture in loaded:
            frames=data.get('cpu_frames',{}).get('frames',0)
            by_name={}
            for zone in data['zones']:
                by_name[zone['name']]=by_name.get(zone['name'],0)+zone['self_ms'].get('sum',0)
            for name in ['bgfx/Render thread wait','bgfx/Metal frame latency wait',
                         'bgfx/Metal acquire drawable','Render.prepareSceneResources',
                         'Render.profileMemory','Render.profileStats','Host.systemsTick']:
                if name in by_name and frames:
                    rows.append([escape(sample_name(path,capture)),marker(name),
                        number(by_name[name]/frames,missing)])
        body+=heading('paired_observations')+'<p>'+locale['paired_note']+'</p>'
        body+=table([locale['scope'],locale['marker'],locale['self_per_frame']+' ms'],rows)
    for path,data,capture in loaded:
        body+='<section><h2>'+escape(sample_name(path,capture))+'</h2>'
        meta=capture.get('metadata',{})
        settings=meta.get('actualSettings',{}).get('native',{}).get('render',{})
        metadata_rows=[]
        for key in ['head','branch','dirtyDiffSha256','binarySha256','os','architecture','backend',
                    'build','logicalWindow','warmupFrames']:
            if key in meta:
                metadata_rows.append([locale['metadata'][key],escape(meta[key])])
        for key in ['size','msaa','vsync','targetFps']:
            if key in settings:
                item=settings[key]
                if isinstance(item,bool):
                    item=locale['modes']['on' if item else 'off']
                metadata_rows.append([locale['metadata'][key],escape(item)])
        if metadata_rows:
            body+='<details><summary>'+locale['conditions']+'</summary>'+\
                table([locale['metric'],locale['value']],metadata_rows)+'</details>'
        body+='<p>'+locale['window']+': '+number(data['window']['seconds'],missing)+' · '+\
            locale['frames']+': '+str(data['frame_ms']['count'])+'</p>'
        body+=table([locale['frame_ms'],locale['mean'],locale['p50'],locale['p95'],locale['p99'],locale['max']],
            [['ms',*[value(data['frame_ms'],key) for key in ['mean','p50','p95','p99','max']]]])
        intervals=data.get('frame_intervals',[])
        if intervals:
            points=[entry['duration_ms'] for entry in intervals]
            maximum=max(max(points),1)
            xy=' '.join(f'{i/max(len(points)-1,1)*1000:.2f},{180-v/maximum*170:.2f}' for i,v in enumerate(points))
            body+=heading('frame_curve')+'<p>0 — '+number(maximum,missing)+' ms</p>'+\
                '<svg role="img" aria-label="'+locale['frame_curve']+'" viewBox="0 0 1000 190" preserveAspectRatio="none">'+\
                '<polyline fill="none" stroke="currentColor" stroke-width="1.5" points="'+xy+'"/></svg>'
        cpu=data.get('cpu_frames',{})
        coverage=data.get('coverage',{})
        body+=heading('coverage')
        body+=table([locale['metric'],locale['value']],[[locale['host_frames'],cpu.get('frames',missing)],
            [locale['direct_coverage'],number(cpu.get('direct_stage_coverage_percent'),missing)+' %'],
            [locale['bgfx_callbacks'],coverage.get('bgfx_callback_events',missing)],
            [locale['excluded'],data.get('excluded_zone_events',missing)]])
        body+='<p>'+locale['scope_note']+'</p>'
        if not coverage.get('bgfx_callback_events'):
            body+='<p class="warning">'+locale['coverage_missing']+'</p>'
        body+=heading('cpu_tree')+'<p>'+locale['cpu_tree_note']+'</p>'
        def tree(node,depth=0):
            text='<details class="tree"'+(' open' if depth<2 else '')+'><summary>'+label(node['name'])+\
                ' <strong>'+number(node['per_frame_ms'],missing)+' ms</strong></summary>'
            text+='<p>'+locale['self_per_frame']+': '+number(node['self_per_frame_ms'],missing)+' ms · '+\
                locale['calls']+': '+str(node['calls'])+'</p><code>'+escape(node['name'])+'</code>'
            text+='<small>'+locale['source']+': '+escape(node['source'])+':'+str(node['line'])+'</small>'
            text+=''.join(tree(child,depth+1) for child in node.get('children',[]))+'</details>'
            return text
        body+=''.join(tree(root) for root in cpu.get('roots',[]))
        body+=heading('threads')+'<p>'+locale['thread_note']+'</p>'
        body+=table([locale['thread'],locale['covered']+' ms',locale['wait_related']+' ms',locale['window_percent']],
            [[escape(row['thread'])+' / '+(locale['main_thread'] if row['main'] else locale['worker_thread']),
              number(row['covered_ms'],missing),number(row['wait_related_ms'],missing),
              number(row['covered_window_percent'],missing)+' %'] for row in cpu.get('threads',[])])
        body+=heading('hotspots')+'<input type="search" class="filter" placeholder="'+locale['search']+'">'
        rows=[]
        for zone in sorted(data['zones'],key=lambda row:row['self_ms'].get('sum',0),reverse=True)[:50]:
            rows.append([marker(zone['name'])+'<small>'+locale['source']+': '+escape(zone['source'])+':'+str(zone['line'])+'</small>',
                escape(zone['thread']),zone['self_ms'].get('count',0),
                number(zone['self_ms'].get('sum',0)/max(cpu.get('frames',0),1),missing),value(zone['self_ms'])])
        body+=table([locale['marker'],locale['thread'],locale['calls'],locale['self_per_frame']+' ms',locale['call_mean']+' ms'],rows,'hotspots')
        views=data.get('renderer_views',{})
        body+=heading('gpu_views')+'<p>'+locale['gpu_note']+'</p>'
        rows=[]
        for view in views.get('views',[]):
            source_frames=view.get('gpu_source_frames',[])
            rows.append([view['view_id'],escape(locale['groups'][view['group']])+'<code>'+escape(view['name'])+'</code>',
                view['gpu_ms'].get('count',0) if view['gpu_available'] else missing,
                value(view['gpu_ms']),value(view['gpu_ms'],'p95'),value(view['cpu_ms']),
                f'{source_frames[0]} — {source_frames[-1]}' if source_frames else missing])
        body+=table([locale['view'],locale['pass'],locale['sample_count'],locale['gpu_mean']+' ms',
            locale['gpu_p95']+' ms',locale['cpu_mean']+' ms',locale['source_frames']],rows)
        absent=views.get('missing_gpu_view_ids',[])
        body+='<p>'+locale['gpu_missing']+': '+(escape(', '.join(map(str,absent))) if absent else '—')+'</p>'
        body+='<p>'+locale['gpu_frame_span']+': '+value(views.get('gpu_command_buffer_ms',{}))+' ms</p>'
        body+='<p>'+locale['gpu_frame_note']+'</p>'
        body+=heading('memory')+'<p>'+locale['memory_note']+'</p>'
        rss=capture.get('rss_summary_bytes') or {}
        rows=[[locale[label_key],number(rss[key]/1048576,missing)+' MiB' if key in rss else missing]
              for key,label_key in [('first','rss_first'),('max','rss_peak'),('last','rss_last')]]
        for key,text in locale['plots'].items():
            stats=data['plots'].get(key,{})
            val=stats.get('mean')
            is_bytes=key.endswith('Bytes')
            rows.append([escape(text)+'<code>'+escape(key)+'</code>',
                number(val/1048576 if is_bytes and val is not None else val,missing)+(' MiB' if is_bytes and val is not None else '')])
        body+=table([locale['metric'],locale['value']],rows)
        body+=heading('queue')
        groups=data.get('task_queue',{}).get('groups',[])
        if groups:
            body+=table([locale['domain'],locale['task'],locale['queued']+' ms',locale['execution']+' ms'],
                [[escape(row['domain']),escape(row['task']),value(row['queued_ms']),value(row['execution_ms'])] for row in groups])
        else:body+='<p>'+locale['queue_empty']+'</p>'
        body+=heading('conclusions')
        if data['zones']:
            top=max(data['zones'],key=lambda row:row['self_ms'].get('sum',0))
            body+='<p>'+locale['largest_self']+': '+marker(top['name'])+' / '+locale['thread']+' '+escape(top['thread'])+'</p>'
        gpu=[view for view in views.get('views',[]) if view['gpu_available']]
        if gpu:
            top=max(gpu,key=lambda row:row['gpu_ms'].get('mean',0))
            body+='<p>'+locale['largest_gpu']+': '+escape(locale['groups'][top['group']])+' / '+\
                escape(top['name'])+' / '+value(top['gpu_ms'])+' ms</p>'
        body+=heading('repro')+'<p>'+locale['repro_note']+'</p><code>'+escape(data['trace'])+\
            '</code><code>SHA256: '+escape(data['trace_sha256'])+'</code><code>'+escape(path)+'</code></section>'
    css='''body{font:16px/1.6 system-ui,sans-serif;max-width:1100px;margin:24px auto;padding:0 16px;background:#f5f6f8;color:#20252b}h1{font-size:1.7rem}h2{font-size:1.25rem;margin-top:28px}section{border-top:2px solid #8193a4;margin-top:24px}table{border-collapse:collapse;width:100%;font-size:14px}td,th{text-align:left;vertical-align:top;padding:8px;border-bottom:1px solid #c7cfd8}code,small{display:block;font-size:12px;overflow-wrap:anywhere}code{font-family:ui-monospace,monospace;color:#495b70}.tree{border-left:2px solid #b5c2d0;padding-left:10px;margin:10px 0}.tree p{font-size:13px;margin:4px 0}summary{cursor:pointer}input{font:inherit;max-width:100%;box-sizing:border-box}svg{width:100%;height:180px;color:#3376b8}.warning{color:#a0411c}@media(prefers-color-scheme:dark){body{background:#171b21;color:#e1e7ee}code{color:#b5cce6}td,th{border-color:#566270}.warning{color:#ffbe8d}svg{color:#82b8ee}}@media(max-width:600px){body{padding:0 10px}table,tbody,tr,td{display:block}thead{display:none}tr{border-bottom:2px solid #9aabbc;margin-bottom:12px}td:before{content:attr(data-label)": ";font-weight:650}.tree{padding-left:6px}h1{font-size:1.4rem}}'''
    script='''for(const input of document.querySelectorAll('.filter')){input.oninput=()=>{for(const row of input.nextElementSibling.querySelectorAll('tbody tr'))row.hidden=!row.textContent.toLowerCase().includes(input.value.toLowerCase());};}'''
    document='<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>'+locale['title']+'</title><style>'+css+'</style><body>'+body+'<script>'+script+'</script></body></html>'
    output=Path(output)
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(document,encoding='utf-8')
    return output


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--analysis',type=Path,action='append',required=True,help='A summary.json file')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    print(write_reports(args.analysis,args.output))
