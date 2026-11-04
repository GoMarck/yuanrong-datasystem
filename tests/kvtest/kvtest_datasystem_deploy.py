"""Optional rc44 standalone/JF orchestration; native DS operations stay unchanged."""
import json
from pathlib import Path
import re
import sys
import time

from kvtest_scenario import NODE_NAME

ROOT = Path(__file__).resolve().parent


def jf_settings(scenario, plan):
    deployment = scenario.get('datasystem_deployment', {})
    if (not isinstance(deployment, dict) or set(deployment) - {'mode', 'jf'}
            or deployment.get('mode', 'dscli') not in ('dscli', 'standalone_jf')):
        raise ValueError('datasystem_deployment requires mode dscli or standalone_jf')
    if deployment.get('mode', 'dscli') == 'dscli':
        if 'jf' in deployment:
            raise ValueError('datasystem_deployment.jf requires standalone_jf mode')
        return None
    config = deployment.get('jf', {})
    if not isinstance(config, dict) or set(config) - {'node', 'port', 'service', 'ttl_seconds'}:
        raise ValueError('datasystem_deployment.jf has unknown fields')
    node = config.get('node', scenario.get('control_node') or plan.placement('datasystem', 'coordinator')[0][0])
    port = config.get('port', 9999)
    ttl = config.get('ttl_seconds', 30)
    service = config.get('service', 'kvcache_coordinator')
    if not isinstance(node, str) or len(node) > 253 or not NODE_NAME.fullmatch(node):
        raise ValueError('datasystem_deployment.jf.node must be a Kubernetes node name')
    if type(port) is not int or not 0 < port < 65536:
        raise ValueError('datasystem_deployment.jf.port must be a valid port')
    if type(ttl) is not int or ttl < 3:
        raise ValueError('datasystem_deployment.jf.ttl_seconds must be at least 3')
    if not isinstance(service, str) or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_.-]*', service):
        raise ValueError('datasystem_deployment.jf.service must be a service identifier')
    return {'node': node, 'port': port, 'service': service, 'ttl_seconds': ttl}


def native_package(scenario):
    artifacts = scenario.get('artifacts', {})
    binary = artifacts.get('kvtest_binaries', {}).get('datasystem', artifacts.get('kvtest_binary', 'output/kvtest'))
    return (ROOT / binary).resolve().parent


def check_native_package(scenario):
    package = native_package(scenario)
    for name in ('coordinator_test', 'worker_test', 'lib/libdatasystem.so',
                 'lib/libdatasystem_coordinator.so', 'lib/libdatasystem_worker.so'):
        if not (package / name).is_file():
            raise RuntimeError(f'JF standalone requires {package / name}; use the complete native DS output package')


def datasystem_service_args(scenario, plan, role, jf_address, whl):
    jf = jf_settings(scenario, plan)
    if jf is None:
        if not whl:
            raise ValueError('dscli deployment requires --ds-whl or artifacts.datasystem_whl')
        return ['--whl', whl]
    package = native_package(scenario)
    args = ['--standalone', '--jf', jf_address, '--service', jf['service'],
            '--binary', package / f'{role}_test', '--lib-dir', package / 'lib',
            '--remote-dir', f'/data/standalone/{role}']
    if role == 'coordinator':
        args += ['--ttl', str(jf['ttl_seconds']), '--expected-member-count',
                 str(sum(count for _, count in plan.placement('datasystem', 'coordinator')))]
    return args


def start_jf_mock(run, scenario, plan, namespace, image, template):
    jf = jf_settings(scenario, plan)
    run.deploy_pods(namespace, 'ds-jf', image, template,
                    f"{run.addresses[jf['node']]}:1", '1', '1Gi')
    pods = run.pods(namespace, 'ds-jf-', 1)
    pod = next(iter(pods.values()))
    address = f"{pod['status']['podIP']}:{jf['port']}"
    run.command([sys.executable, ROOT / 'deploy_jf.py', 'deploy', '-n', namespace, '-p', 'ds-jf-',
                 '--port', str(jf['port']), '--ttl-default', str(jf['ttl_seconds']),
                 '--remote-dir', '/data/jf'], timeout=120)
    return {'mode': 'standalone_jf', 'jf': jf, 'jf_address': address,
            'jf_pod': pod['metadata']['name'], 'native_package': str(native_package(scenario))}


def wait_jf_registration(run, namespace, discovery, coordinators, timeout=30):
    address = f"127.0.0.1:{discovery['jf']['port']}"
    url = f"http://{address}/discover/{discovery['jf']['service']}"
    probe = ('from urllib.request import build_opener, ProxyHandler\n'
             f'print(build_opener(ProxyHandler({{}})).open({url!r}, timeout=3).read().decode())')
    deadline = time.monotonic() + timeout
    last_error = 'no instances registered'
    while time.monotonic() < deadline:
        try:
            result = run.command(['kubectl', '-n', namespace, 'exec', discovery['jf_pod'], '--',
                                  'python3', '-c', probe], capture=True, timeout=10)
            snapshot = json.loads(result)
            if set(coordinators).issubset(set(snapshot.get('instances', []))):
                return snapshot
            last_error = f'expected {coordinators}, got {snapshot}'
        except (RuntimeError, OSError, ValueError) as error:
            last_error = str(error)
        time.sleep(1)
    raise RuntimeError(f'JF coordinator registration timed out: {last_error}')
