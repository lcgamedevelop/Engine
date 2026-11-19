"""Join exported Metal GPU intervals to named encoders in a process-scoped recording."""

from collections import Counter, defaultdict
from pathlib import Path
import re
from xml.etree import ElementTree

from analysis_views import interval_union


def table_rows(path, schema_name, columns):
    try:
        root = ElementTree.parse(path).getroot()
    except ElementTree.ParseError as error:
        raise ValueError(f'Invalid Metal table XML: {path.name}') from error
    node = root.find('node')
    schema = node.find('schema') if node is not None else None
    if schema is None or schema.get('name') != schema_name:
        raise ValueError(f'Unexpected Metal table schema: {path.name}')
    names = [column.findtext('name') for column in schema.findall('col')]
    if any(column not in names for column in columns):
        raise ValueError(f'Metal table is missing required columns: {path.name}')
    indices = [names.index(column) for column in columns]
    definitions = {value.get('id'): value for value in node.iter() if 'id' in value.attrib}
    for row in node.findall('row'):
        if len(row) != len(names):
            raise ValueError(f'Invalid Metal table row: {path.name}')
        values = []
        for index in indices:
            element = row[index]
            if 'ref' in element.attrib:
                element = definitions[element.attrib['ref']]
            values.append(element.text or '')
        yield values


def metal_encoder_timings(directory: Path, statistics_of):
    encoder_schema = 'metal-application-encoders-list'
    gpu_schema = 'metal-gpu-intervals'
    encoders = {}
    for encoder, frame, label, event in table_rows(directory / f'{encoder_schema}.xml',
            encoder_schema, ('Encoder Id', 'Frame', 'Encoder', 'Event Type')):
        if event != 'Encoding' or not encoder or not frame:
            continue
        name = re.fullmatch(r'\s*(\d+)C?\s+(.+)', label)
        if name:
            encoders[encoder] = (int(frame), int(name[1]), name[2])

    if not encoders:
        raise ValueError('Metal recording has no labelled bgfx encoders')

    labels = defaultdict(Counter)
    view_frames = defaultdict(list)
    for encoder, begin, duration, channel, state in table_rows(directory / f'{gpu_schema}.xml',
            gpu_schema, ('Encoder Id', 'Creation', 'Duration', 'Channel Name', 'State')):
        if encoder not in encoders or state != 'Active' or not channel:
            continue
        start_ns, duration_ns = int(begin), int(duration)
        if start_ns < 0 or duration_ns <= 0:
            raise ValueError('Invalid Metal GPU execution interval')
        frame, view, name = encoders[encoder]
        labels[view, channel][name] += 1
        view_frames[frame, view, channel].append((start_ns, start_ns + duration_ns))

    if not view_frames:
        raise ValueError('Metal recording has no active GPU intervals for the labelled encoders')

    names = {key: counts.most_common(1)[0][0] for key, counts in labels.items()}
    views = defaultdict(list)
    pass_frames = defaultdict(list)
    for (frame, view, channel), intervals in view_frames.items():
        views[view, channel].append(interval_union(intervals) / 1e6)
        pass_frames[frame, names[view, channel]].extend(intervals)
    passes = defaultdict(list)
    for (_, name), intervals in pass_frames.items():
        passes[name].append(interval_union(intervals) / 1e6)
    return {
        'frames': len({frame for frame, _, _ in view_frames}),
        'passes': sorted(({'name': name, 'execution_ms': statistics_of(values)}
                          for name, values in passes.items()),
                         key=lambda item: -item['execution_ms']['p50']),
        'views': sorted(({'view_id': view, 'name': names[view, channel],
                          'channel': channel,
                          'execution_ms': statistics_of(values)}
                         for (view, channel), values in views.items()),
                        key=lambda item: (-item['execution_ms']['p50'], item['view_id'])),
    }
