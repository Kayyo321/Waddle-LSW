#!/usr/bin/env python3
"""Regression tests for exact-shape, bounded, concurrent coverage recording."""
import json
import os
from pathlib import Path
import signal
import resource
import runpy
import sys
from unittest.mock import patch
import subprocess
import tempfile
import unittest

from coverage_recorder import prepare_recorder, RecorderBytes, clear_instrumentation_summaries

Root = Path(__file__).resolve().parents[2]
Runtime = Root / 'tests/device_branch_runtime.c'
Sanitizers = ['-fsanitize=address,leak,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'] if os.environ.get('WADDLE_RECORDER_SANITIZERS') == '1' else []


def probe_environment(output):
    """Keep ordinary probes unchanged; sanitizer probes cannot inherit suppression.

    Replace the complete option strings rather than appending to potentially
    conflicting inherited flags. Leak scanning stays enabled and reports fail.
    """
    environment = {**os.environ, 'WADDLE_BRANCH_OUT': str(output)}
    if Sanitizers:
        environment.update(
            ASAN_OPTIONS='detect_leaks=1:halt_on_error=1:abort_on_error=1',
            LSAN_OPTIONS='detect_leaks=1:leak_check_at_exit=1:exitcode=23',
            UBSAN_OPTIONS='halt_on_error=1:abort_on_error=1:print_stacktrace=1')
    return environment


def records_for(count):
    return [{'function': 'probe', 'line': index + 1, 'column': 1, 'edges': 1}
            for index in range(count)]


def ir_for(records):
    return '\n'.join(f'  call void @waddle_branch_hit(i32 {index}, i32 0)'
                     for index in range(len(records))) + '\ndeclare void @waddle_branch_hit(i32, i32)\n'


class recorder_test_t(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='waddle-recorder-')
        self.addCleanup(self.temporary.cleanup)
        self.output = Path(self.temporary.name)

    def prepare(self, records, raw_ir='original native IR', instrumented=None):
        return prepare_recorder(self.output, raw_ir, instrumented or ir_for(records), records,
                                {'zig_version': 'test', 'secret_environment': 'must not persist'})

    def compile(self, text, argument=None, success=True):
        source = self.output / 'probe.c'
        source.write_text(text)
        command = [os.environ.get('CC', 'cc'), '-D_GNU_SOURCE', '-std=c11', '-Wall', '-Wextra',
                   '-Wpedantic', '-Werror', '-O2', *Sanitizers, str(source), str(Runtime), '-pthread',
                   '-o', str(self.output / 'probe')]
        if argument:
            command.append(argument)
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode == 0, success, result.stderr)
        return result

    def run_probe(self, *arguments):
        return subprocess.run([str(self.output / 'probe'), *arguments],
                              env=probe_environment(self.output),
                              capture_output=True, text=True,
                              preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)), timeout=30)

    @unittest.skipUnless(Sanitizers, 'sanitizer-only non-recovery qualification')
    def test_sanitizer_violation_is_nonrecoverable(self):
        self.assertIn('-fno-sanitize-recover=all', Sanitizers)
        self.compile("""#include <limits.h>
#include <stdio.h>
int main(void) {
    volatile int maximum = INT_MAX;
    volatile int invalid = maximum + 1;
    (void)invalid;
    fputs("RECOVERED_AFTER_UBSAN\\n", stderr);
    return 0;
}
""")
        # Host settings must not disable qualification or inject suppressions.
        with patch.dict(os.environ, {
                'ASAN_OPTIONS': 'detect_leaks=0:halt_on_error=0:suppressions=/unusable',
                'LSAN_OPTIONS': 'detect_leaks=0:exitcode=0:suppressions=/unusable',
                'UBSAN_OPTIONS': 'halt_on_error=0:suppressions=/unusable'}):
            environment = probe_environment(self.output)
            self.assertEqual(environment['ASAN_OPTIONS'], 'detect_leaks=1:halt_on_error=1:abort_on_error=1')
            self.assertEqual(environment['LSAN_OPTIONS'], 'detect_leaks=1:leak_check_at_exit=1:exitcode=23')
            self.assertEqual(environment['UBSAN_OPTIONS'], 'halt_on_error=1:abort_on_error=1:print_stacktrace=1')
            result = self.run_probe()
        self.assertEqual(result.returncode, -signal.SIGABRT, result.stderr)
        self.assertIn('runtime error:', result.stderr)
        self.assertIn('signed integer overflow', result.stderr)
        self.assertNotIn('RECOVERED_AFTER_UBSAN', result.stderr)
        self.assertNotIn('LeakSanitizer has encountered a fatal error', result.stderr)

    def test_every_site_above_old_limit_and_concurrent_edge_union(self):
        count = 76512
        records = records_for(count)
        records[0]['edges'] = records[-1]['edges'] = 32
        instrumented, argument = self.prepare(records)
        manifest = json.loads((self.output / 'recorder.json').read_text())
        symbol = manifest['recorder_symbol']
        self.assertNotIn('secret_environment', manifest)
        self.assertEqual(manifest['sites'], count)
        self.assertEqual(manifest['estimated_storage_bytes'], 382560)
        self.assertLess(manifest['estimated_storage_bytes'], RecorderBytes)
        self.assertEqual(instrumented.count('call void @' + symbol), count)
        self.compile('''#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/resource.h>
void ''' + symbol + '''(uint32_t, uint32_t);
static pthread_barrier_t barrier;
static void *record(void *value) {
    uint32_t edge = (uint32_t)(uintptr_t)value;
    (void)pthread_barrier_wait(&barrier);
    for (unsigned index = 0; index < 1000; index++) ''' + symbol + '''(0, edge);
    return NULL;
}
int main(int argc, char **argv) {
    const struct rlimit no_core = {0, 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    if (argc > 1) {
        if (!strcmp(argv[1], "site")) ''' + symbol + f'''({count}, 0);
        if (!strcmp(argv[1], "overflow")) ''' + symbol + '''(UINT32_MAX, 0);
        if (!strcmp(argv[1], "edge")) ''' + symbol + '''(0, 32);
        if (!strcmp(argv[1], "arity")) ''' + symbol + '''(1, 1);
        return 0;
    }
    for (uint32_t index = 0; index < ''' + str(count) + '''; index++) ''' + symbol + '''(index, 0);
    ''' + symbol + '(' + str(count - 1) + ''', 31);
    pthread_t threads[32];
    if (pthread_barrier_init(&barrier, NULL, 32)) return 1;
    for (uintptr_t index = 0; index < 32; index++)
        if (pthread_create(&threads[index], NULL, record, (void *)index)) return 2;
    for (unsigned index = 0; index < 32; index++)
        if (pthread_join(threads[index], NULL)) return 3;
    return pthread_barrier_destroy(&barrier);
}
''', argument)
        result = self.run_probe()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('382560/524288 bytes; 76512 sites', result.stderr)
        hits = {tuple(map(int, line.split())) for path in self.output.glob('*.edges')
                for line in path.read_text().splitlines()}
        expected = {(index, 0) for index in range(count)} | {(0, edge) for edge in range(32)} | {(count - 1, 31)}
        self.assertEqual(hits, expected)
        for failure in ('site', 'overflow', 'edge', 'arity'):
            result = self.run_probe(failure)
            self.assertEqual(result.returncode, -signal.SIGABRT, result.stderr)
            self.assertIn('Coverage recorder', result.stderr)

    def test_metadata_count_arity_calls_and_budget(self):
        for records in ([], [{'edges': 0}], [{'edges': 33}], [{'edges': -1}], [{'edges': True}]):
            with self.assertRaises(ValueError):
                self.prepare(records)
        valid = ir_for(records_for(1))
        for ir, error in (
            (valid.replace('i32 0, i32 0', 'i32 1, i32 0'), 'exact metadata index'),
            (valid.replace('i32 0, i32 0', 'i32 0, i32 1'), 'invalid constant edge'),
            (valid.replace('i32 0, i32 0', 'i32 0, i32 -1'), 'invalid constant edge'),
            (valid.split('declare void')[0], 'one exact declaration'),
            (valid + 'declare void @waddle_branch_hit(i32, i32)\n', 'one exact declaration')):
            with self.assertRaisesRegex(ValueError, error):
                self.prepare(records_for(1), instrumented=ir)
            self.assertEqual(json.loads((self.output / 'recorder.json').read_text())['validation'], 'failed')
        for malformed in ('  call void @waddle_branch_hit(i32 -1, i32 0)',
                          '  call void @waddle_branch_hit(i32 %bad, i32 0)',
                          '  call void @waddle_branch_hit(i32 0, i32 true)',
                          '  call void @waddle_branch_hit(i32 0, i32 poison)',
                          '  call void @waddle_branch_hit_extra(i32 0, i32 0)'):
            with self.assertRaisesRegex(ValueError, 'malformed'):
                self.prepare(records_for(1), instrumented=ir_for(records_for(1)) + malformed)
        with self.assertRaisesRegex(ValueError, 'unchanged budget'):
            self.prepare(records_for(104858))
        class huge_records_t:
            def __len__(self):
                return 0x100000000
        with self.assertRaisesRegex(ValueError, 'uint32'):
            prepare_recorder(self.output, "ir", "", huge_records_t(), {})
        _, argument = self.prepare(records_for(104857))
        symbol = json.loads((self.output / 'recorder.json').read_text())['recorder_symbol']
        self.compile('#include <stdint.h>\nvoid ' + symbol + '(uint32_t,uint32_t);\n'
                     'int main(void) {' + symbol + '(104856,0);return 0;}\n', argument)
        result = self.run_probe()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('524288/524288 bytes', result.stderr)
        header = self.output / 'recorder_metadata.h'
        header.write_text(header.read_text().replace('104857u', '104858u'))
        result = self.compile('int main(void) {return 0;}\n', argument, success=False)
        self.assertIn('storage budget', result.stderr)

    def test_digest_binding_and_malformed_header(self):
        records = records_for(3)
        _, argument = self.prepare(records)
        original = json.loads((self.output / 'recorder.json').read_text())['recorder_symbol']
        self.prepare(records, raw_ir='different native IR')
        changed = json.loads((self.output / 'recorder.json').read_text())['recorder_symbol']
        self.assertNotEqual(original, changed)
        result = self.compile('#include <stdint.h>\nvoid ' + original + '(uint32_t,uint32_t);\n'
                              'int main(void) {' + original + '(0,0);return 0;}\n', argument, success=False)
        self.assertIn(original, result.stderr)
        records[0]['edges'] = 2
        self.prepare(records)
        self.assertNotEqual(original, json.loads((self.output / 'recorder.json').read_text())['recorder_symbol'])
        for invalid in ('0', '33'):
            self.prepare(records_for(3))
            header = self.output / 'recorder_metadata.h'
            header.write_text(header.read_text().replace('1, 1, 1,', '1, ' + invalid + ', 1,'))
            self.compile('int main(void) {return 0;}\n', argument)
            result = self.run_probe()
            self.assertEqual(result.returncode, -signal.SIGABRT, result.stderr)
            self.assertIn('metadata: site=1 arity=' + invalid, result.stderr)

    def test_zero_site_harness_writes_failure_manifest(self):
        source = self.output / 'src/av/av_audio.zig'
        source.parent.mkdir(parents=True)
        source.write_text('pub fn probe() void {}\n')
        def compile_stub(command, **kwargs):
            self.assertEqual(command[:2], ['zig', 'test'])
            ir_path = Path(next(arg.split('=', 1)[1] for arg in command if arg.startswith('-femit-llvm-ir=')))
            ir_path.write_text('target triple = "x86_64-unknown-linux-gnu"\n')
        previous = Path.cwd()
        try:
            os.chdir(self.output)
            with patch.object(sys, 'argv', ['coverage.py', 'av_audio']), \
                 patch('subprocess.run', side_effect=compile_stub), \
                 patch('subprocess.check_output', return_value='test compiler\n'):
                with self.assertRaisesRegex(ValueError, 'nonzero uint32'):
                    runpy.run_path(str(Root / 'tests/av/coverage.py'))
        finally:
            os.chdir(previous)
        manifests = list(self.output.glob('build/coverage/av/av_audio/*/recorder.json'))
        self.assertEqual(len(manifests), 1)
        manifest = json.loads(manifests[0].read_text())
        self.assertEqual((manifest['sites'], manifest['validation']), (0, 'failed'))

    def test_attribute_cleanup_preserves_metadata_parameters_and_strings(self):
        fixture = ('define i32 @probe(ptr readonly %memory) memory(none) nosync willreturn nofree nounwind {\n'
                   '  call void @callee(ptr readonly %memory) memory(argmem: read) speculatable nounwind\n'
                   '  ret i32 0\n}\n'
                   'attributes #0 = { memory(none) speculatable nosync willreturn nofree nounwind uwtable "target-cpu"="znver3" "label"="memory(none) nofree" }\n'
                   '!0 = !{!"memory(none) nofree nosync willreturn"}\n')
        result = clear_instrumentation_summaries(fixture)
        self.assertIn('ptr readonly %memory', result)
        self.assertIn('nounwind uwtable "target-cpu"="znver3"', result)
        self.assertIn('"label"="memory(none) nofree"', result)
        self.assertEqual(result.splitlines()[-1], fixture.splitlines()[-1])
        self.assertNotIn(') memory(', result)
        self.assertNotIn('{ memory(', result)

    def test_instrumented_llvm_runtime_round_trip(self):
        records = records_for(16385)
        raw_ir = 'define i32 @main() #0 {\nentry:\n  ret i32 0\n}\n'
        instrumented = ('define i32 @main() #0 {\nentry:\n' +
                        ir_for(records).split('declare void')[0] +
                        '  ret i32 0\n}\ndeclare void @waddle_branch_hit(i32, i32)\n'
                        'attributes #0 = { memory(none) speculatable nosync willreturn nofree nounwind }\n')
        instrumented, argument = self.prepare(records, raw_ir, instrumented)
        ir_path = self.output / 'instrumented.ll'
        ir_path.write_text(instrumented)
        obj = self.output / 'instrumented.o'
        subprocess.run(['clang-19', '-O2', '-Wno-override-module', '-c', str(ir_path), '-o', str(obj)], check=True, timeout=120)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Wpedantic',
                        '-Werror', *Sanitizers, str(obj), str(Runtime), argument, '-o', str(self.output / 'probe')], check=True, timeout=120)
        result = self.run_probe()
        self.assertEqual(result.returncode, 0, result.stderr)
        hits = {tuple(map(int, line.split())) for path in self.output.glob('*.edges')
                for line in path.read_text().splitlines()}
        self.assertEqual(hits, {(index, 0) for index in range(16385)})

    def test_legacy_abi_preserves_last_edge(self):
        self.compile('#include <stdint.h>\nvoid waddle_branch_hit(uint32_t,uint32_t);\n'
                     'int main(void) {waddle_branch_hit(16383,31);return 0;}\n')
        result = self.run_probe()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([path.read_text() for path in self.output.glob('*.edges')], ['16383 31\n'])


if __name__ == '__main__':
    unittest.main()
