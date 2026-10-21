#!/usr/bin/env python3
"""Portable source-extracted test of OpenCL transport-loss teardown ordering."""
import ast
import hashlib
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

Root = Path(__file__).resolve().parents[2]
Source = Root / 'tests/vgpu/opencl_integration.c'
Extractor = Root / 'tests/vgpu/clvk_build.py'
Fixture = Path(__file__).with_name('opencl_cancellation_order_fixture.c')
Compiler = shlex.split(__import__('os').environ.get('CC', 'cc'))


def extract_function(source, signature):
    """Extract one exact balanced definition without importing build side effects."""
    tree = ast.parse(Extractor.read_text(encoding='utf-8'))
    function = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                    and node.name == 'extract_cpp_definition')
    namespace = {}
    exec(compile(ast.Module(body=[function], type_ignores=[]), str(Extractor), 'exec'), namespace)
    return namespace['extract_cpp_definition'](source, signature)


def make_header(source):
    """Wrap exact actual definitions in portable ABI declarations and test seams."""
    signatures = (
        'typedef struct transport_loss_call_t {',
        'static unsigned __stdcall run_transport_loss_call(void *opaque)',
        'static int release_transport_loss_owners(const api_t *api, cl_event *event,',
        'static void shutdown_transport_loss(const api_t *api, clvk_shutdown_t shutdown)',
        'static int release_transport_loss_after_retirement(const api_t *api,',
    )
    parts = []
    for signature in signatures:
        definition = extract_function(source, signature)
        if signature.startswith('typedef struct'):
            terminator = ' transport_loss_call_t;'
            start = source.index(signature)
            end = start + len(definition)
            if source[end:end + len(terminator)] != terminator:
                raise ValueError('transport loss call type terminator changed')
            definition += terminator
        parts.append(definition)
    extracted = '\n\n'.join(parts) + '\n'
    source_digest = hashlib.sha256(source.encode('utf-8')).hexdigest()
    extraction_digest = hashlib.sha256(extracted.encode('utf-8')).hexdigest()
    content = '''/* Generated from exact current transport-loss function bodies. */
#ifndef TransportLossOrderActualH
#define TransportLossOrderActualH
#define __stdcall
#include <stddef.h>
#include <stdint.h>
typedef uint64_t ULONGLONG;
typedef int32_t cl_int;
typedef uint32_t cl_uint;
typedef struct platform_t *cl_platform_id;
typedef struct fake_handle_t *cl_command_queue;
typedef struct fake_handle_t *cl_kernel;
typedef struct fake_handle_t *cl_mem;
typedef struct fake_handle_t *cl_program;
typedef struct fake_handle_t *cl_context;
typedef struct fake_handle_t *cl_event;
typedef struct api_t {
 cl_int (*enqueue_kernel)(cl_command_queue,cl_kernel,cl_uint,const size_t *,const size_t *,const size_t *,cl_uint,const cl_event *,cl_event *);
 cl_int (*read_buffer)(cl_command_queue,cl_mem,uint32_t,size_t,size_t,void *,cl_uint,const cl_event *,cl_event *);
 cl_int (*release_event)(cl_event); cl_int (*release_kernel)(cl_kernel);
 cl_int (*release_buffer)(cl_mem); cl_int (*release_program)(cl_program);
 cl_int (*release_queue)(cl_command_queue); cl_int (*release_context)(cl_context);
 cl_int (*get_platform_ids)(cl_uint,cl_platform_id *,cl_uint *);
} api_t;
typedef int32_t (*clvk_shutdown_t)(uint32_t);
typedef int32_t venus_ring_status_t;
typedef venus_ring_status_t (*bootstrap_abandon_t)(uint64_t);
enum { CL_SUCCESS=0, CL_INVALID_OPERATION=-59, CL_OUT_OF_RESOURCES=-5,
 CL_INVALID_PLATFORM=-32, CL_TRUE=1, RingOk=0, ClvkShutdownAbi=1,
 ClvkShutdownVersion=2, ClvkShutdownBusy=3, ClvkShutdownOk=0 };
#define ActualIntegrationSourceSha256 "''' + source_digest + '''"
#define ActualExtractionSha256 "''' + extraction_digest + '''"
static void fail_closed(const char *message);
static void fake_wait_retired_bounded(const wchar_t *path,uint64_t identity,ULONGLONG timeout_ms);
static ULONGLONG GetTickCount64(void);
static void Sleep(unsigned long milliseconds);
#define wait_retired_bounded fake_wait_retired_bounded
''' + extracted + '''\n#endif\n'''
    return content, extracted


def main():
    """Own fresh evidence; require sanitizers and >=90% actual helper coverage."""
    import gzip
    import json
    import os
    import handle_diag_coverage as gate
    source = Source.read_text(encoding='utf-8')
    generated, extracted = make_header(source)
    parent = Root / 'build/coverage/vgpu/opencl_cancellation_order'
    parent.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='run-', dir=parent))
    header = out / 'transport_loss_order_actual.h'
    header.write_text(generated, encoding='utf-8')
    resolved = Path(shutil.which(Compiler[0])).resolve()
    inputs = (Source, Extractor, Fixture, Path(__file__).resolve(), resolved,
              Path(shutil.which('gcov')).resolve(), Root / 'tests/vgpu/handle_diag_coverage.py')
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    before = {str(path): digest(path) for path in inputs}
    receipt = {'inputs_before': before, 'generated_header_sha256': digest(header),
               'extracted_sha256': hashlib.sha256(extracted.encode()).hexdigest(),
               'artifact_directory': str(out), 'native_vm_gpu_execution': False, 'runs': []}
    def execute(command, env=None):
        result = subprocess.run(command, cwd=out, env=env, text=True,
                                capture_output=True, timeout=60)
        receipt['runs'].append({'command': command, 'exit': result.returncode,
                               'stdout': result.stdout, 'stderr': result.stderr})
        if result.returncode:
            raise RuntimeError('command failed: ' + str(command) + '\n' + result.stderr)
        return result
    common = [*Compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wpedantic',
              '-I', str(out)]
    try:
        for optimization in ('-O0', '-O2'):
            binary = out / ('fixture_' + optimization[1:])
            execute([*common, optimization, str(Fixture), '-o', str(binary)])
            execute([str(binary)])
        binary = out / 'fixture_sanitized'
        execute([*common, '-O1', '-g', '-fsanitize=address,leak,undefined',
                 '-fno-omit-frame-pointer', str(Fixture), '-o', str(binary)])
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1:halt_on_error=1',
                   LSAN_OPTIONS='exitcode=23', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        receipt['sanitizer_environment'] = {key: env[key] for key in ('ASAN_OPTIONS','LSAN_OPTIONS','UBSAN_OPTIONS')}
        execute([str(binary)], env)
        obj = out / 'fixture.o'
        execute([*common, '-O0', '-g', '--coverage', '-fprofile-abs-path',
                 '-c', str(Fixture), '-o', str(obj)])
        execute([*Compiler, '--coverage', str(obj), '-o', str(out / 'fixture_coverage')])
        execute([str(out / 'fixture_coverage')])
        execute(['gcov', '--json-format', '-b', '-c', str(out / 'fixture.gcno')])
        raw = out / 'fixture.gcov.json.gz'
        report = json.loads(gzip.decompress(raw.read_bytes()))
        gate.CoverageFiles = (str(Fixture), str(header))
        gate.CoverageSource = str(header)
        summary = gate.coverage_summary(report, Root)
        actual = next(item for item in report['files'] if item['file']==str(header))
        expected = {'run_transport_loss_call','release_transport_loss_owners',
                    'shutdown_transport_loss','release_transport_loss_after_retirement'}
        functions = actual.get('functions', [])
        if len(functions)!=len(expected) or {f.get('name') for f in functions}!=expected or any(
                type(f.get('execution_count')) is not int or f['execution_count']<=0 for f in functions):
            raise ValueError('missing, duplicate or unexecuted actual function')
        receipt['coverage'] = dict(summary, functions=len(functions),
                                   gcov_sha256=digest(raw), minimum_percent=90)
    finally:
        receipt['inputs_after'] = {str(path): digest(path) for path in inputs}
        receipt['generated_header_sha256_after'] = digest(header)
        (out / 'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
        print('Cancellation order evidence:', out, flush=True)
    if receipt['inputs_after']!=before or digest(header)!=receipt['generated_header_sha256']:
        raise ValueError('source/tool/extracted header changed during verification')
    print(json.dumps(receipt['coverage'],sort_keys=True))


if __name__ == '__main__':
    main()
