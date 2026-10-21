"""Compile controls against exact transport-loss definitions extracted from OpenCL integration."""
import ast
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest


Root = Path(__file__).resolve().parents[2]
IntegrationSource = Root / 'tests/vgpu/opencl_integration.c'
ExtractionHelper = Root / 'tests/vgpu/clvk_build.py'
FixtureSource = Root / 'tests/vgpu/opencl_cancellation_fixture.c'
ArtifactRoot = Root / 'build/qualification_feature_20261021'
CompilerCommand = shlex.split(os.environ.get('CC', 'cc'))
CompilerBinary = Path(shutil.which(CompilerCommand[0])).resolve()
CoverageTool = Path(shutil.which('gcov')).resolve()


def load_definition_extractor():
    """Load the existing brace-aware extractor without importing build-driver side effects."""
    tree = ast.parse(ExtractionHelper.read_text(encoding='utf-8'))
    function = next(node for node in tree.body
                    if isinstance(node, ast.FunctionDef) and
                    node.name == 'extract_cpp_definition')
    namespace = {}
    exec(compile(ast.Module(body=[function], type_ignores=[]),
                 str(ExtractionHelper), 'exec'), namespace)
    return namespace['extract_cpp_definition']


def extract_actual_definitions(source_text):
    """Return exact production type and functions, preserving their source text."""
    extract = load_definition_extractor()
    struct_signature = 'typedef struct transport_loss_call_t {'
    struct_start = source_text.index(struct_signature)
    struct_definition = extract(source_text, struct_signature)
    struct_end = struct_start + len(struct_definition)
    struct_terminator = ' transport_loss_call_t;'
    if source_text[struct_end:struct_end + len(struct_terminator)] != struct_terminator:
        raise ValueError('transport_loss_call_t terminator changed')
    struct_definition += struct_terminator
    run_definition = extract(source_text,
        'static unsigned __stdcall run_transport_loss_call(void *opaque)')
    release_definition = extract(source_text,
        'static int release_transport_loss_owners(const api_t *api, cl_event *event,')
    return '\n\n'.join((struct_definition, run_definition, release_definition)) + '\n'


def create_artifact_dir():
    ArtifactRoot.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix='opencl_cancellation_subagent_', dir=ArtifactRoot))


def generated_header(source_digest, extracted):
    extraction_digest = hashlib.sha256(extracted.encode('utf-8')).hexdigest()
    return '''/* Generated from tests/vgpu/opencl_integration.c by opencl_cancellation_test.py. */
#ifndef OpenclCancellationActualH
#define OpenclCancellationActualH
#include <stddef.h>
#include <stdint.h>
#define __stdcall
#define CL_SUCCESS 0
#define CL_TRUE 1
#define CL_INVALID_OPERATION -59
#define CL_OUT_OF_RESOURCES -5
typedef int32_t cl_int;
typedef uint32_t cl_uint;
typedef uint32_t cl_bool;
struct fake_handle_t;
typedef struct fake_handle_t *cl_command_queue;
typedef struct fake_handle_t *cl_kernel;
typedef struct fake_handle_t *cl_mem;
typedef struct fake_handle_t *cl_program;
typedef struct fake_handle_t *cl_context;
typedef struct fake_handle_t *cl_event;
typedef struct api_t {
    cl_int (*enqueue_kernel)(cl_command_queue,cl_kernel,cl_uint,const size_t *,
        const size_t *,const size_t *,cl_uint,const cl_event *,cl_event *);
    cl_int (*read_buffer)(cl_command_queue,cl_mem,cl_bool,size_t,size_t,void *,
        cl_uint,const cl_event *,cl_event *);
    cl_int (*release_event)(cl_event);
    cl_int (*release_kernel)(cl_kernel);
    cl_int (*release_buffer)(cl_mem);
    cl_int (*release_program)(cl_program);
    cl_int (*release_queue)(cl_command_queue);
    cl_int (*release_context)(cl_context);
} api_t;
#define ActualIntegrationSourceSha256 "''' + source_digest + '''"
#define ActualExtractionSha256 "''' + extraction_digest + '''"
''' + extracted + '''
#endif
'''


def sha256_file(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_command(command, artifact_dir, environment=None):
    return subprocess.run(command, cwd=artifact_dir, env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)


def append_run(receipt, name, command, compile_result, run_result=None):
    item = {'name': name, 'compile_command': command,
        'compile_returncode': compile_result.returncode,
        'compile_stdout': compile_result.stdout,
        'compile_stderr': compile_result.stderr}
    if run_result is not None:
        item.update({'run_returncode': run_result.returncode,
            'run_stdout': run_result.stdout, 'run_stderr': run_result.stderr})
    receipt['runs'].append(item)
    return item


class cancellation_controls_t(unittest.TestCase):
    def test_extracted_transport_loss_controls_plain_and_sanitized(self):
        source_bytes = IntegrationSource.read_bytes()
        source_text = source_bytes.decode('utf-8')
        extracted = extract_actual_definitions(source_text)
        source_digest = hashlib.sha256(source_bytes).hexdigest()
        extraction_digest = hashlib.sha256(extracted.encode('utf-8')).hexdigest()
        artifact_dir = create_artifact_dir()
        actual_header_path = artifact_dir / 'transport_loss_actual.h'
        actual_header_path.write_text(generated_header(source_digest, extracted), encoding='utf-8')
        input_paths = {
            'integration_source': IntegrationSource,
            'fixture_source': FixtureSource,
            'extractor_source': ExtractionHelper,
            'test_script': Path(__file__).resolve(),
            'resolved_compiler': CompilerBinary,
            'resolved_coverage_tool': CoverageTool,
        }
        input_hashes_before = {name: sha256_file(path) for name, path in input_paths.items()}
        generated_header_digest = sha256_file(actual_header_path)
        receipt = {
            'inputs': {name: str(path) for name, path in input_paths.items()},
            'input_hashes_before': input_hashes_before,
            'compiler_command': CompilerCommand,
            'coverage_tool_command': [str(CoverageTool)],
            'integration_source': str(IntegrationSource),
            'integration_source_sha256': source_digest,
            'extracted_definitions_sha256': extraction_digest,
            'generated_header_sha256': generated_header_digest,
            'fixture_source': str(FixtureSource),
            'extractor_source': str(ExtractionHelper),
            'artifact_directory': str(artifact_dir),
            'runs': [],
        }
        receipt_path = artifact_dir / 'receipt.json'

        plain_binary = artifact_dir / 'fixture-plain'
        plain_command = [*CompilerCommand, '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-Wpedantic', '-O2', '-I', str(artifact_dir), str(FixtureSource),
            '-o', str(plain_binary)]
        plain_compile = run_command(plain_command, Root)
        plain_run = run_command([str(plain_binary)], Root) if plain_compile.returncode == 0 else None
        append_run(receipt, 'plain', plain_command, plain_compile, plain_run)

        sanitized_binary = artifact_dir / 'fixture-asan_lsan_ubsan'
        sanitized_command = [*CompilerCommand, '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-Wpedantic', '-O1', '-g', '-fsanitize=address,leak,undefined',
            '-fno-omit-frame-pointer', '-I', str(artifact_dir), str(FixtureSource),
            '-o', str(sanitized_binary)]
        sanitized_compile = run_command(sanitized_command, Root)
        sanitizer_environment = os.environ.copy()
        sanitizer_environment['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1:halt_on_error=1'
        sanitizer_environment['LSAN_OPTIONS'] = 'exitcode=23'
        sanitizer_environment['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
        receipt['sanitizer_environment'] = {
            key: sanitizer_environment[key] for key in
            ('ASAN_OPTIONS', 'LSAN_OPTIONS', 'UBSAN_OPTIONS')}
        sanitized_run = run_command([str(sanitized_binary)], Root, sanitizer_environment) \
            if sanitized_compile.returncode == 0 else None
        append_run(receipt, 'asan_lsan_ubsan', sanitized_command, sanitized_compile, sanitized_run)

        coverage_object = artifact_dir / 'fixture-coverage.o'
        coverage_binary = artifact_dir / 'fixture-coverage'
        coverage_compile_command = [*CompilerCommand, '-std=c11', '-Wall', '-Wextra',
            '-Werror', '-Wpedantic', '-O0', '--coverage', '-I', str(artifact_dir),
            '-c', str(FixtureSource), '-o', str(coverage_object)]
        coverage_compile = run_command(coverage_compile_command, Root)
        coverage_link_command = [*CompilerCommand, '--coverage', str(coverage_object),
            '-o', str(coverage_binary)]
        coverage_link = run_command(coverage_link_command, Root) if coverage_compile.returncode == 0 else None
        coverage_run = run_command([str(coverage_binary)], Root) if coverage_link and coverage_link.returncode == 0 else None
        coverage_record = append_run(receipt, 'coverage',
            {'compile': coverage_compile_command, 'link': coverage_link_command},
            coverage_compile, coverage_run)
        coverage_record['link_returncode'] = coverage_link.returncode if coverage_link else None
        coverage_record['link_stdout'] = coverage_link.stdout if coverage_link else ''
        coverage_record['link_stderr'] = coverage_link.stderr if coverage_link else ''
        gcov_command = [str(CoverageTool), '-b', '-c', '-f', '-o', str(coverage_object),
            str(actual_header_path)]
        gcov_result = run_command(gcov_command, artifact_dir) if coverage_run and coverage_run.returncode == 0 else None
        coverage_record['gcov_command'] = gcov_command
        coverage_record['gcov_returncode'] = gcov_result.returncode if gcov_result else None
        coverage_record['gcov_stdout'] = gcov_result.stdout if gcov_result else ''
        coverage_record['gcov_stderr'] = gcov_result.stderr if gcov_result else ''
        coverage_metrics = {}
        if gcov_result and gcov_result.returncode == 0:
            target_heading = "File '" + str(actual_header_path) + "'"
            target_start = gcov_result.stdout.rfind(target_heading)
            target_summary = gcov_result.stdout[target_start:] if target_start>=0 else ''
            line_match = re.search(r"Lines executed:\s*([0-9.]+)% of ([0-9]+)", target_summary)
            branch_match = re.search(r"Branches executed:\s*([0-9.]+)% of ([0-9]+)", target_summary)
            covered_functions = {
                name: re.search(r"Function '" + name + r"'\s+Lines executed:\s*([0-9.]+)%", gcov_result.stdout)
                for name in ('run_transport_loss_call', 'release_transport_loss_owners')}
            if line_match:
                coverage_metrics['line_percent'] = float(line_match.group(1))
                coverage_metrics['line_count'] = int(line_match.group(2))
            if branch_match:
                coverage_metrics['branch_percent'] = float(branch_match.group(1))
                coverage_metrics['branch_count'] = int(branch_match.group(2))
            coverage_metrics['functions_line_percent'] = {
                name: float(match.group(1)) if match else None
                for name, match in covered_functions.items()}
        coverage_record['coverage_metrics'] = coverage_metrics
        receipt['generated_header_sha256_after_commands'] = sha256_file(actual_header_path)
        receipt['input_hashes_after'] = {name: sha256_file(path) for name, path in input_paths.items()}
        receipt['all_inputs_unchanged_after_commands'] = receipt['input_hashes_before'] == receipt['input_hashes_after']
        receipt_path.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')

        self.assertEqual(plain_compile.returncode, 0, plain_compile.stderr)
        self.assertIsNotNone(plain_run)
        self.assertEqual(plain_run.returncode, 0, plain_run.stderr)
        self.assertIn(source_digest, plain_run.stdout)
        self.assertIn(extraction_digest, plain_run.stdout)
        self.assertEqual(coverage_compile.returncode, 0, coverage_compile.stderr)
        self.assertIsNotNone(coverage_link)
        self.assertEqual(coverage_link.returncode, 0, coverage_link.stderr)
        self.assertIsNotNone(coverage_run)
        self.assertEqual(coverage_run.returncode, 0, coverage_run.stderr)
        self.assertIn(source_digest, coverage_run.stdout)
        self.assertIn(extraction_digest, coverage_run.stdout)
        self.assertIsNotNone(gcov_result)
        self.assertEqual(gcov_result.returncode, 0, gcov_result.stderr)
        self.assertIn('line_percent', coverage_metrics)
        self.assertIn('branch_percent', coverage_metrics)
        self.assertGreaterEqual(coverage_metrics['line_percent'], 90.0)
        self.assertGreaterEqual(coverage_metrics['branch_percent'], 90.0)
        self.assertTrue(all(value is not None and value >= 90.0
            for value in coverage_metrics['functions_line_percent'].values()), coverage_metrics)
        self.assertTrue(receipt['all_inputs_unchanged_after_commands'], receipt['input_hashes_after'])
        self.assertEqual(receipt['generated_header_sha256_after_commands'], generated_header_digest)
        self.assertEqual(sanitized_compile.returncode, 0, sanitized_compile.stderr)
        self.assertIsNotNone(sanitized_run)
        self.assertEqual(sanitized_run.returncode, 0, sanitized_run.stderr)
        self.assertIn(source_digest, sanitized_run.stdout)
        self.assertIn(extraction_digest, sanitized_run.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
