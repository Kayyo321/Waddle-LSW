"""Build private, exact-shape recorder metadata without changing measured sites."""
import hashlib
import json
from pathlib import Path
import re

MaxEdges = 32
RecorderBytes = 512 * 1024
SchemaVersion = 1


def clear_instrumentation_summaries(ir):
    """Remove invalidated function summaries only, preserving strings/parameters.

    All groups are conservative because callers inherit callee summaries. nofree
    is also invalid on diagnostic paths through stdio/abort. Parameter contracts,
    CPU strings, nounwind and stack protection remain unchanged.
    """
    quoted = r'("(?:\\.|[^"\\])*")'
    tokens = re.compile(r'(?<![\w.$@%])(?:memory\([^()]*\)|speculatable|nosync|willreturn|nofree)(?![\w.$])')
    def clean(fragment):
        pieces = re.split(quoted, fragment)
        return ''.join(piece if index % 2 else tokens.sub('', piece)
                       for index, piece in enumerate(pieces))
    result = []
    for line in ir.splitlines(keepends=True):
        if re.match(r'^attributes #\d+ = \{', line):
            prefix, body = line.split('{', 1)
            line = prefix + '{' + clean(body)
        elif re.match(r'^(?:define |declare |\s*(?:%[^ ]+ = )?(?:(?:tail|musttail|notail) )?(?:call|invoke|callbr) )', line):
            # Only the suffix after the callee argument list contains function
            # attributes. Nested parameter expressions and quoted names survive.
            callee = re.search(r'(?:@(?:[\w.$-]+|"(?:\\.|[^"\\])*")|%[\w.$-]+)\s*\(', line)
            if callee:
                masked = re.sub(quoted, lambda match: ' ' * len(match[0]), line)
                depth = 1
                for position in range(callee.end(), len(masked)):
                    depth += (masked[position] == '(') - (masked[position] == ')')
                    if depth == 0:
                        line = line[:position + 1] + clean(line[position + 1:])
                        break
        result.append(line)
    return ''.join(result)


def prepare_recorder(output, raw_ir, instrumented, records, diagnostics):
    """Own generated files in output; return digest-bound IR and compiler argument.

    Recorder arrays are statically owned; no heap allocation is introduced.
    Invalid or over-budget
    metadata raises ValueError; every original record and call must be retained.
    The C sizeof assertion verifies the actual ABI rather than trusting this
    four-byte atomic-mask estimate.
    """
    output = Path(output)
    count = len(records)
    manifest = {
        'schema': SchemaVersion, 'sites': count, 'max_edges': MaxEdges,
        'storage_budget_bytes': RecorderBytes, 'validation': 'pending',
        # Explicit whitelist: never copy arbitrary environment or diagnostics.
        **{key: diagnostics[key] for key in ('source_sha256', 'compile_command',
            'zig_version', 'clang_version', 'target_triple', 'target_cpus',
            'target_features') if key in diagnostics},
    }
    def fail(message):
        manifest.update(validation='failed', error=message)
        (output / 'recorder.json').write_text(json.dumps(manifest, indent=2) + '\n')
        raise ValueError(message)
    if not count or count > 0xffffffff:
        fail('coverage site count must fit nonzero uint32')
    if any(type(record) is not dict or type(record.get('edges')) is not int or
           not 1 <= record['edges'] <= MaxEdges for record in records):
        fail('coverage metadata arity must be an integer in 1..32')
    # Four-byte masks followed by byte arities, rounded for four-byte alignment.
    storage_bytes = (count * 5 + 3) // 4 * 4
    records_bytes = json.dumps(records, indent=2).encode()
    records_hash = hashlib.sha256(records_bytes).hexdigest()
    ir_hash = hashlib.sha256(raw_ir.encode()).hexdigest()
    binding = hashlib.sha256(f'{SchemaVersion}:{ir_hash}:{records_hash}'.encode()).hexdigest()
    symbol = 'waddle_branch_hit_' + binding
    manifest.update(estimated_storage_bytes=storage_bytes,
                    records_sha256=records_hash, ir_sha256=ir_hash,
                    binding_sha256=binding, recorder_symbol=symbol)
    (output / 'recorder.json').write_text(json.dumps(manifest, indent=2) + '\n')
    if storage_bytes > RecorderBytes:
        fail(f'coverage recorder needs {storage_bytes} bytes for {count} sites; '
                         f'unchanged budget is {RecorderBytes}')
    calls = []
    declarations = 0
    for line in instrumented.splitlines():
        if '@waddle_branch_hit' not in line:
            continue
        if line == 'declare void @waddle_branch_hit(i32, i32)':
            declarations += 1
            continue
        call = re.fullmatch(r'  call void @waddle_branch_hit\(i32 (\d+), i32 (-?\d+|%[A-Za-z0-9_.$]+)\)', line)
        if call is None:
            fail('malformed coverage recorder reference')
        calls.append(call.groups())
    if declarations != 1:
        fail('coverage recorder requires one exact declaration')
    if {int(index) for index, _ in calls} != set(range(count)):
        fail('coverage instrumentation must reference every exact metadata index')
    for index, edge in calls:
        if re.fullmatch(r'-?\d+', edge) and not 0 <= int(edge) < records[int(index)]['edges']:
            fail('coverage instrumentation has an invalid constant edge')
    arities = [str(record['edges']) for record in records]
    rows = ['    ' + ', '.join(arities[start:start + 32]) + ','
            for start in range(0, count, 32)]
    header = ('/* Generated private coverage metadata; immutable exact record order. */\n'
              f'#define WaddleCoverageSiteCount {count}u\n'
              f'#define WaddleCoverageHitSymbol {symbol}\n'
              '#define WaddleCoverageArities { \\\n' +
              ' \\\n'.join(rows) + ' \\\n}\n')
    header_path = output / 'recorder_metadata.h'
    header_path.write_text(header)
    manifest['validation'] = 'validated'
    (output / 'recorder.json').write_text(json.dumps(manifest, indent=2) + '\n')
    result = clear_instrumentation_summaries(instrumented).replace('@waddle_branch_hit(', '@' + symbol + '(')
    return result, '-DWaddleCoverageMetadataHeader="' + str(header_path.resolve()) + '"'
