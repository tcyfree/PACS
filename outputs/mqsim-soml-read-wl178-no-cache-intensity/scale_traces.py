from concurrent.futures import ProcessPoolExecutor
from contextlib import ExitStack
from hashlib import sha256
import json
from pathlib import Path
from tempfile import TemporaryDirectory


OUTPUT = Path(__file__).resolve().parent
PILOT = OUTPUT.parent / 'mqsim-soml-read-wl178-no-cache-pacs-wl-ablation'
TRACE_NAMES = ('1612_2', 'src2_2', 'hm_0', 'web_0')
FACTORS = (1, 2, 5, 10, 20, 50)
FORMULA = 't0 + factor * (timestamp - t0)'


def file_sha256(path):
    digest = sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(2 ** 20), b''):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, data):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(data, indent=2, allow_nan=False) + '\n')
    temporary.replace(path)


def scale(name, original, factors=FACTORS, output=OUTPUT):
    source = Path(original['path'])
    info = source.stat()
    assert (info.st_size, info.st_mtime_ns) == (original['size'], original['mtime_ns']), name
    destinations = {factor: output / 'scaled' / f'{factor}x' / 'sortedTrace' / source.name for factor in factors[1:]}
    for destination in destinations.values():
        destination.parent.mkdir(parents=True, exist_ok=True)

    payload_digest = sha256()
    digests = {factor: sha256() for factor in factors}
    first, previous = None, 0
    ties, writes, reads = 0, 0, 0
    print(f'{name}: preparing {len(destinations)} timestamp-scaled copies', flush=True)
    with TemporaryDirectory(prefix=f'.{name}-', dir=output) as workdir:
        staging = {factor: Path(workdir) / f'{factor}x.csv' for factor in destinations}
        with ExitStack() as opened:
            source_file = opened.enter_context(source.open('rb', buffering=2 ** 20))
            outputs = {factor: opened.enter_context(path.open('wb', buffering=2 ** 20)) for factor, path in staging.items()}
            for line in source_file:
                time, space, payload = line.partition(b' ')
                fields = payload.split()
                assert time.isdigit() and space and len(fields) == 4 and fields[-1] in (b'0', b'1'), (name, line)
                timestamp = int(time)
                assert timestamp >= previous, (name, timestamp, previous)
                if first is None:
                    first = timestamp
                else:
                    ties += timestamp == previous
                previous = timestamp
                writes += fields[-1] == b'0'
                reads += fields[-1] == b'1'
                payload_digest.update(payload)
                digests[1].update(line)
                for factor, destination in outputs.items():
                    scaled_line = str(first + factor * (timestamp - first)).encode('ascii') + space + payload
                    destination.write(scaled_line)
                    digests[factor].update(scaled_line)
        assert first is not None and previous - first == original['span_ns'], name
        assert (reads, writes) == (original['reads'], original['writes']), name
        assert first + max(factors) * (previous - first) <= 2 ** 63 - 1, name
        assert (source.stat().st_size, source.stat().st_mtime_ns) == (info.st_size, info.st_mtime_ns), name
        assert all(file_sha256(staging[factor]) == digests[factor].hexdigest() for factor in staging), name
        for factor, path in staging.items():
            path.replace(destinations[factor])

    scaled = {}
    for factor, path in {1: source, **destinations}.items():
        physical = path.stat()
        scaled[str(factor)] = {
            **original, 'path': str(path), 'size': physical.st_size, 'mtime_ns': physical.st_mtime_ns,
            'sha256': digests[factor].hexdigest(), 'payload_sha256': payload_digest.hexdigest(),
            'first_timestamp_ns': first, 'last_timestamp_ns': first + factor * (previous - first),
            'span_ns': factor * (previous - first), 'zero_interval_pairs': ties,
        }
    print(f'{name}: {reads + writes:,} requests, {ties:,} zero intervals, exact payload preserved', flush=True)
    return scaled


def prepare_traces(*, output=OUTPUT, factors=FACTORS, trace_names=TRACE_NAMES, originals=None, jobs=None):
    if originals is None:
        originals = json.loads((PILOT / 'experiment.json').read_text())['traces']
    output.mkdir(parents=True, exist_ok=True)
    path = output / 'trace_manifest.json'
    if path.exists():
        manifest = json.loads(path.read_text())
        assert manifest['factors'] == list(factors) and manifest['timestamp_transform'] == FORMULA
        assert tuple(manifest['traces']) == tuple(trace_names)
        for name, by_factor in manifest['traces'].items():
            assert set(by_factor) == {str(factor) for factor in factors}
            assert all(by_factor['1'][key] == value for key, value in originals[name].items())
            for details in by_factor.values():
                file = Path(details['path'])
                info = file.stat()
                assert (info.st_size, info.st_mtime_ns) == (details['size'], details['mtime_ns']), file
                assert file_sha256(file) == details['sha256'], file
        return manifest
    with ProcessPoolExecutor(max_workers=min(len(trace_names), jobs or len(trace_names))) as pool:
        pending = {name: pool.submit(scale, name, originals[name], factors, output) for name in trace_names}
        traces = {name: job.result() for name, job in pending.items()}
    manifest = {'factors': list(factors), 'time_unit': 'NANOSECOND', 'timestamp_transform': FORMULA, 'traces': traces}
    write_json(path, manifest)
    return manifest


if __name__ == '__main__':
    prepare_traces()
