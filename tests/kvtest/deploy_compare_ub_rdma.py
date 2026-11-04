#!/usr/bin/env python3
"""Run JSON-configured Kubernetes KVTest comparisons for DataSystem UB and Mooncake RDMA.

Uses the existing deploy_pods/deploy_coordinator/deploy_worker/deploy_mooncake/
deploy_client commands. Run on the Kubernetes master after placing both SDK
outputs under this directory and building output/kvtest with --with-mooncake.
"""

import argparse
import copy
import csv
import gzip
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import threading
import time
import uuid
from urllib.request import build_opener, ProxyHandler
from urllib.error import URLError
from kvtest_report import api_operations, api_label, api_boundary_note
from kvtest_environment import validate_environment
from kvtest_datasystem_deploy import (jf_settings, check_native_package, datasystem_service_args,
                                      start_jf_mock, wait_jf_registration)
from ds_config_template import render_ds_config, RUNTIME_CONFIG_KEYS
from kvtest_scenario import (Plan, DEFAULT_DATA_SIZE, SIZE_UNITS, data_size_bytes,
                             scenario_size, build_plan, expand_scenarios, configured_systems)


ROOT = Path(__file__).resolve().parent
SCENARIO = ROOT / 'comparison' / 'scenario.two-rdma-nodes-8clients.json'
DS_IMAGE = 'docker.io/library/kvcache:B043-85b27d03e'


def selected_backends(systems):
    choices = {'both': ('datasystem', 'mooncake'),
               'datasystem': ('datasystem',), 'mooncake': ('mooncake',)}
    if systems not in choices:
        raise ValueError(f'invalid systems: {systems}')
    return choices[systems]


def pod_image_for(backend, mc_image):
    if backend not in ('datasystem', 'mooncake'):
        raise ValueError(f'invalid backend: {backend}')
    return mc_image


def component_environment(scenario, backend, role):
    return validate_environment(scenario['components'][backend][role].get('env', {}),
                                f'{backend} {role}.env', ('POD_IP', 'HOST_IP'))


def mooncake_store_env_args(scenario, case_dir):
    env = component_environment(scenario, 'mooncake', 'store')
    if not env:
        return []
    path = case_dir / 'store-env.json'
    write_json(path, env)
    return ['--env-json', str(path)]


def mooncake_master_args(scenario, case_dir):
    flags = dict(scenario['components']['mooncake']['master'].get('args', {}))
    flags.pop('rpc_port', None)
    if not flags:
        return []
    path = case_dir / 'master-args.json'
    write_json(path, flags)
    return ['--args-json', str(path)]


def worker_reference(scenario):
    path = scenario['components']['datasystem']['worker'].get('config_file')
    if not path:
        return None
    reference = json.loads((ROOT / path).read_text(encoding='utf-8'))
    if not isinstance(reference, dict) or any(not isinstance(item, dict) or 'value' not in item
                                              for item in reference.values()):
        raise ValueError('worker config_file must contain dscli entries with value fields')
    return reference


def strict_ds_config(scenario, role, address):
    component = scenario['components']['datasystem'][role]
    if not component.get('config_file'):
        raise ValueError(f'{role}: strict template mode requires config_file')
    source = (ROOT / component['config_file']).read_bytes().decode('utf-8')
    config = json.loads(source)
    updates = dict(component.get('config', {}))
    updates['coordinator_address'] = address
    if role == 'worker':
        updates['cluster_name'] = scenario['cluster_name']
        expected = scenario['memory_gib']['datasystem_worker'] * 1024
        if int(config['shared_memory_size_mb']['value']) != expected:
            raise ValueError('memory_gib.datasystem_worker must match strict worker template '
                             'shared_memory_size_mb; template capacity is not overridden')
        if str(config['enable_urma']['value']).lower() != 'true':
            raise ValueError('DataSystem UB comparison requires enable_urma=true in the supplied template')
    patch_ds_config(config, updates)
    return source, render_ds_config(source, config, RUNTIME_CONFIG_KEYS)


def ds_template_args(scenario, role):
    return (['--preserve-config-template']
            if scenario['components']['datasystem'][role].get('preserve_config_template', False) else [])


def rdma_device_preference(scenario, pod):
    rdma = scenario['mooncake_rdma']
    return rdma.get('devices_by_node', {}).get(pod['spec']['nodeName'], rdma.get('device_name', 'auto'))


def validate_client_options(scenario, backend):
    config = scenario['components'][backend]['client'].get('config', {})
    if not isinstance(config, dict):
        raise ValueError(f'{backend}.client.config must be an object')
    if backend == 'mooncake':
        if config not in ({}, {'request_timeout_seconds': 30}):
            raise ValueError('Mooncake client.config is unsupported by this adapter; use an empty object and client.env')
        return
    allowed = {'enable_local_cache', 'enable_cross_node_connection', 'connect_timeout_ms', 'request_timeout_ms',
               'request_timeout_seconds', 'service_discovery', 'host_id_env_name', 'data_placement_policy',
               'fast_transport_mem_size'}
    discovery = 'jf' if jf_settings(scenario, build_plan(scenario)) else 'coordinator'
    if set(config) - allowed or config.get('service_discovery', discovery) != discovery:
        raise ValueError('DataSystem client.config has unsupported fields or service_discovery')
    for key in ('enable_local_cache', 'enable_cross_node_connection'):
        if key in config and type(config[key]) is not bool:
            raise ValueError(f'{key} must be boolean')
    for key in ('connect_timeout_ms', 'request_timeout_ms', 'request_timeout_seconds'):
        if key in config and (type(config[key]) is not int or config[key] < (1 if key == 'connect_timeout_ms' else 0)):
            raise ValueError(f'{key} must be a valid nonnegative integer timeout')
    if 'request_timeout_ms' in config and 'request_timeout_seconds' in config:
        raise ValueError('configure only one of request_timeout_ms and request_timeout_seconds')
    if not isinstance(config.get('host_id_env_name', 'HOST_IP'), str) or not re.fullmatch(
            r'[A-Za-z_][A-Za-z0-9_]*', config.get('host_id_env_name', 'HOST_IP')):
        raise ValueError('host_id_env_name must be an environment variable name')
    if 'fast_transport_mem_size' in config:
        data_size_bytes(config['fast_transport_mem_size'])
    if config.get('data_placement_policy', 'PREFERRED_SAME_NODE') not in (
            'PREFERRED_SAME_NODE', 'REQUIRED_SAME_NODE', 'PREFERRED_META_OWNER'):
        raise ValueError('data_placement_policy must be PREFERRED_SAME_NODE, REQUIRED_SAME_NODE or PREFERRED_META_OWNER')


def plan_from_scenario(scenario):
    expected_protocols = {'datasystem': 'ub', 'mooncake': 'rdma'}
    if any(scenario.get('protocols', {}).get(backend) != expected_protocols[backend]
           for backend in configured_systems(scenario)):
        raise ValueError('selected systems must use DataSystem UB / Mooncake RDMA')
    cluster_name = scenario.get('cluster_name')
    if not isinstance(cluster_name, str) or not cluster_name.strip():
        raise ValueError('scenario requires a nonempty cluster_name for workers and clients')
    worker_cluster = scenario.get('components', {}).get('datasystem', {}).get('worker', {}).get('config', {}).get('cluster_name')
    if worker_cluster is not None and worker_cluster != cluster_name:
        raise ValueError('DataSystem worker cluster_name conflicts with scenario cluster_name')
    numa = scenario.get('client_numa_node')
    if numa is not None and (type(numa) is not int or numa < 0):
        raise ValueError('client_numa_node must be null (random) or a nonnegative integer')
    plan = build_plan(scenario)
    if 'datasystem' in configured_systems(scenario):
        jf_settings(scenario, plan)
        for role in ('coordinator', 'worker'):
            component = scenario['components']['datasystem'][role]
            if type(component.get('preserve_config_template', False)) is not bool:
                raise ValueError(f'{role}.preserve_config_template must be boolean')
            if component.get('preserve_config_template', False):
                strict_ds_config(scenario, role, '127.0.0.1:31511')
            elif role == 'worker':
                worker_reference(scenario)
    for backend in configured_systems(scenario):
        validate_client_options(scenario, backend)
        for role in (*(('coordinator', 'worker') if backend == 'datasystem' else ('master', 'store')), 'client'):
            component_environment(scenario, backend, role)
            component_resources(scenario, backend, role)
        storage_role = 'worker' if backend == 'datasystem' else 'store'
        pool_key = 'datasystem_worker' if backend == 'datasystem' else 'mooncake_store'
        pool = scenario['memory_gib'][pool_key]
        if type(pool) is not int or pool <= 0 or pool >= component_resources(scenario, backend, storage_role)['memory_gib']:
            raise ValueError(f'{pool_key} must be positive and smaller than the storage Pod memory limit')
    if 'mooncake' in configured_systems(scenario):
        buffer_size = scenario['memory_gib']['mooncake_client_buffer']
        buffer_limit = min(component_resources(scenario, 'mooncake', role)['memory_gib'] for role in ('client', 'store'))
        if type(buffer_size) is not int or not 0 < buffer_size < buffer_limit:
            raise ValueError('mooncake_client_buffer must be a positive integer smaller than client/store Pod memory')
        if (scenario['memory_gib']['mooncake_store'] + buffer_size
                >= component_resources(scenario, 'mooncake', 'store')['memory_gib']):
            raise ValueError('Mooncake segment plus local buffer must be smaller than the store Pod memory limit')
        rdma = scenario['mooncake_rdma']
        devices = rdma.get('devices_by_node', {})
        device_nodes = {node for role in ('store', 'client') for node, _ in plan.placement('mooncake', role)}
        if not isinstance(devices, dict) or set(devices) - device_nodes:
            raise ValueError('mooncake_rdma.devices_by_node must map configured store/client nodes to devices')
        for device in [rdma.get('device_name', 'auto'), *devices.values()]:
            if not isinstance(device, str) or not re.fullmatch(r'[A-Za-z0-9_.:-]+', device):
                raise ValueError('RDMA device names must be nonempty identifiers or auto')
        master_args = scenario['components']['mooncake']['master'].get('args', {})
        if not isinstance(master_args, dict) or any(
                not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*', key) or key == 'rpc_address'
                or not isinstance(value, (str, int, float, bool)) or '\0' in str(value)
                for key, value in master_args.items()):
            raise ValueError('Mooncake master.args requires scalar flags without rpc_address')
        port = master_args.get('rpc_port', 50051)
        if type(port) is not int or not 0 < port < 65536:
            raise ValueError('Mooncake master.args.rpc_port must be a valid port')
        if scenario['components']['mooncake']['store'].get('args', {}):
            raise ValueError('Mooncake store.args is unsupported; use memory_gib, generated transport settings and store.env')
        store_env = component_environment(scenario, 'mooncake', 'store')
        client_env = component_environment(scenario, 'mooncake', 'client')
        if store_env.get('MC_NUM_QP_PER_EP') != client_env.get('MC_NUM_QP_PER_EP'):
            raise ValueError('Mooncake QP counts must match in store.env and client.env')
    return plan


def check_memory_budget(plan, scenario, duration):
    # No bounded write count or Mooncake TTL is available in this pipeline.
    # Leave 25% headroom for rollout skew and allocator metadata.
    write_gib = plan.total_clients * plan.qps * duration * data_size_bytes(plan.size) / SIZE_UNITS['GB']
    capacity_gib = min(scenario['memory_gib'][key] * sum(count for _, count in plan.placement(backend, role))
                       for backend, role, key in (('datasystem', 'worker', 'datasystem_worker'),
                                                  ('mooncake', 'store', 'mooncake_store'))
                       if backend in configured_systems(scenario))
    if write_gib > capacity_gib * 0.75:
        raise ValueError(f'planned {plan.size} write volume {write_gib:.1f} GiB exceeds '
                         f'75% of {capacity_gib} GiB storage budget; reduce --duration '
                         'or raise both storage budgets')


def client_cpu_resources(scenario):
    request = scenario['cpus']['client']
    limit = scenario.get('client_cpu_limit', request)
    if type(request) is not int or type(limit) is not int or request <= 0 or limit < request:
        raise ValueError('client CPU limit must be an integer at least equal to the positive CPU request')
    return str(limit), str(request)


def component_resources(scenario, backend, role):
    memory = scenario['memory_gib']
    if role == 'client':
        limit, request = client_cpu_resources(scenario)
        defaults = {'cpu': int(limit), 'requests_cpu': int(request), 'memory_gib': memory['client_pod']}
    elif role in ('coordinator', 'master'):
        defaults = {'cpu': 4, 'memory_gib': 8}
    else:
        key = 'datasystem_pod' if backend == 'datasystem' else 'mooncake_pod'
        defaults = {'cpu': scenario['cpus']['storage'], 'memory_gib': memory[key]}
    overrides = scenario['components'][backend][role].get('resources', {})
    if not isinstance(overrides, dict) or set(overrides) - {'cpu', 'requests_cpu', 'memory_gib', 'requests_memory_gib'}:
        raise ValueError(f'{backend}.{role}.resources has unknown fields')
    result = dict(defaults, **overrides)
    if 'cpu' in overrides and 'requests_cpu' not in overrides:
        result['requests_cpu'] = result['cpu']
    result.setdefault('requests_cpu', result['cpu'])
    result.setdefault('requests_memory_gib', result['memory_gib'])
    if any(type(v) is not int or v <= 0 for v in result.values()):
        raise ValueError(f'{backend}.{role}.resources must contain positive integers')
    if result['requests_cpu'] > result['cpu'] or result['requests_memory_gib'] > result['memory_gib']:
        raise ValueError(f'{backend}.{role}: resource requests cannot exceed limits')
    return result


def replicas_for(plan, backend, role, addresses):
    return ','.join(f'{addresses[node]}:{count}' for node, count in plan.placement(backend, role))


def component_count(plan, backend, role):
    return sum(count for _, count in plan.placement(backend, role))


def deploy_component(run, scenario, plan, namespace, backend, role, image, templates):
    resource = None if role in ('coordinator', 'master') else (
        scenario['ub_resource'] if backend == 'datasystem' else scenario['mooncake_rdma']['resource'])
    template = pod_template(resource, ipc_lock=backend == 'mooncake' and resource is not None)
    env = component_environment(scenario, backend, role)
    env_yaml = ''.join(f'        - name: {key}\n          value: {json.dumps(value)}\n' for key, value in env.items())
    template = template.replace('      env:\n', '      env:\n' + env_yaml)
    path = templates / f'{backend}-{role}.yaml'
    path.write_text(template, encoding='utf-8')
    resources = component_resources(scenario, backend, role)
    prefix = ('ds-' if backend == 'datasystem' else 'mc-') + role
    run.deploy_pods(namespace, prefix, image, path,
                    replicas_for(plan, backend, role, run.addresses), str(resources['cpu']),
                    f"{resources['memory_gib']}Gi", requests_cpu=str(resources['requests_cpu']),
                    requests_memory=f"{resources['requests_memory_gib']}Gi")
    return run.pods(namespace, prefix + '-', component_count(plan, backend, role))


def used_resource(pods, node, resource):
    def requested(container):
        resources = container.get('resources', {})
        return max(int(resources.get('requests', {}).get(resource, '0')),
                   int(resources.get('limits', {}).get(resource, '0')))

    total = 0
    for pod in pods:
        if pod.get('spec', {}).get('nodeName') != node or pod.get('status', {}).get('phase') in ('Succeeded', 'Failed'):
            continue
        containers = pod['spec'].get('containers', [])
        regular = sum(requested(c) for c in containers)
        init = max((requested(c) for c in pod['spec'].get('initContainers', [])), default=0)
        total += max(regular, init)
    return total


def pod_template(device_resource, *, ipc_lock=False):
    """Template placeholders follow deploy_pods.py, keeping role resources isolated."""
    device = f'          {device_resource}: "1"\n' if device_resource else ''
    lock = ('      securityContext:\n        capabilities:\n          add: ["IPC_LOCK"]\n'
            if ipc_lock or device_resource == 'rdma/hca' else '')
    return (
        'apiVersion: v1\nkind: Pod\nmetadata:\n  name: podName\nspec:\n'
        '  nodeName: nodeName\n  restartPolicy: Never\n'
        '  containers:\n    - name: containerName\n      image: imageName\n'
        '      imagePullPolicy: Never\n'
        '      command: ["/bin/sh", "-c", "exec sleep infinity"]\n'
        + lock +
        '      resources:\n        requests:\n          cpu: "requests_cpu"\n'
        '          memory: "requests_memory"\n' + device +
        '        limits:\n          cpu: "limits_cpu"\n'
        '          memory: "limits_memory"\n' + device +
        '      env:\n        - name: POD_IP\n          valueFrom:\n'
        '            fieldRef:\n              fieldPath: status.podIP\n'
        '        - name: HOST_IP\n          valueFrom:\n'
        '            fieldRef:\n              fieldPath: status.hostIP\n'
        '      volumeMounts:\n        - name: data\n          mountPath: /data\n'
        '        - name: shm\n          mountPath: /dev/shm\n'
        '  volumes:\n    - name: data\n      emptyDir: {}\n'
        '    - name: shm\n      emptyDir:\n        medium: Memory\n'
    )


def patch_ds_config(template, updates):
    unsupported = sorted(k for k in updates if k not in template or
                         not isinstance(template[k], dict) or 'value' not in template[k])
    if unsupported:
        raise ValueError('unsupported image config keys: ' + ', '.join(unsupported))
    for key, value in updates.items():
        template[key]['value'] = str(value).lower() if isinstance(value, bool) else str(value)
    return template


def patch_client_deploy(deploy, host_ips, devices, duration):
    for node in deploy['nodes']:
        name = node['pod_name']
        node['host'] = host_ips[name]
        if devices is not None:
            node['mooncake_device'] = devices[name]
    # Preserve rc44 native stop behavior; the controller owns the wall-time window.
    deploy['duration'] = '0'
    deploy['enable_procmon'] = False
    return deploy


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


class Run:
    def __init__(self, output):
        self.output = output
        self.commands = output / 'commands'
        self.commands.mkdir(parents=True)
        self.step = 0

    def command(self, argv, *, cwd=None, capture=False, timeout=None, display=None):
        self.step += 1
        argv = [str(x) for x in argv]
        name = re.sub(r'[^a-z0-9-]', '-', Path(argv[0]).name.lower())
        log = self.commands / f'{self.step:03d}-{name}.log'
        print('>', display or ' '.join(argv), flush=True)
        if capture:
            result = subprocess.run(argv, cwd=cwd, text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=timeout)
            log.write_text(result.stdout or '', encoding='utf-8', errors='replace')
        else:
            with log.open('w', encoding='utf-8', errors='replace') as output:
                process = subprocess.Popen(argv, cwd=cwd, text=True, stdout=subprocess.PIPE,
                                           stderr=subprocess.STDOUT, errors='replace')
                def relay():
                    for line in process.stdout:
                        output.write(line)
                        output.flush()
                        print(line, end='', flush=True)
                reader = threading.Thread(target=relay, daemon=True)
                reader.start()
                try:
                    returncode = process.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                    reader.join(timeout=5)
                    raise
                reader.join()
                result = subprocess.CompletedProcess(argv, returncode)
        if result.returncode:
            raise RuntimeError(f'command failed ({result.returncode}); see {log}')
        return result.stdout if capture else None

    def kube_json(self, *args):
        return json.loads(self.command(['kubectl', *args, '-o', 'json'], capture=True))

    def pods(self, namespace, prefix, expected):
        items = self.kube_json('-n', namespace, 'get', 'pods')['items']
        pods = {x['metadata']['name']: x for x in items
                if x['metadata']['name'].startswith(prefix)}
        if len(pods) != expected:
            raise RuntimeError(f'{namespace}: expected {expected} {prefix} Pods, found {len(pods)}')
        for name, pod in pods.items():
            self.command(['kubectl', '-n', namespace, 'wait', '--for=condition=Ready',
                          'pod/' + name, '--timeout=300s'], timeout=330)
            if not pod['status'].get('podIP') or not pod['status'].get('hostIP'):
                raise RuntimeError(f'{name}: missing Pod/host IP')
        return pods

    def deploy_pods(self, namespace, prefix, image, template, replicas, cpu, memory, *, requests_cpu=None,
                    requests_memory=None):
        argv = [sys.executable, ROOT / 'deploy_pods.py', 'deploy', '-n', namespace,
                '-p', prefix, '-i', image, '-y', template, '-r', replicas,
                '--cpu', cpu, '--memory', memory, '--wait']
        if requests_cpu is not None:
            argv += ['--requests-cpu', requests_cpu]
        if requests_memory is not None:
            argv += ['--requests-memory', requests_memory]
        self.command(argv, timeout=480)


def node_addresses(run, plan, scenario, backends=('datasystem', 'mooncake')):
    nodes = {x['metadata']['name']: x for x in run.kube_json('get', 'nodes')['items']}
    current_pods = run.kube_json('get', 'pods', '-A')['items']
    addresses = {}
    placements = [(backend, role, plan.placement(backend, role)) for backend in backends
                  for role in (*(('coordinator', 'worker') if backend == 'datasystem' else ('master', 'store')), 'client')]
    jf = jf_settings(scenario, plan) if 'datasystem' in backends else None
    if jf:
        placements.append(('datasystem', 'jf', ((jf['node'], 1),)))
    for name in dict.fromkeys(node for _, _, entries in placements for node, _ in entries):
        node = nodes.get(name)
        if not node or not any(c.get('type') == 'Ready' and c.get('status') == 'True'
                               for c in node['status'].get('conditions', [])):
            raise RuntimeError(f'node {name} is not Ready')
        addresses[name] = next(x['address'] for x in node['status']['addresses']
                               if x['type'] == 'InternalIP')
    for backend in backends:
        resource = scenario['ub_resource'] if backend == 'datasystem' else scenario['mooncake_rdma']['resource']
        demands = {}
        for system, role, entries in placements:
            if system == backend and role in ('worker', 'store', 'client'):
                for node, count in entries:
                    demands[node] = demands.get(node, 0) + count
        for name, count in demands.items():
            alloc = nodes[name]['status']['allocatable']
            available = int(alloc.get(resource, 0)) - used_resource(current_pods, name, resource)
            if available < count:
                raise RuntimeError(f'{name}: {resource} free={available}, need {count} '
                                   f'(allocatable={alloc.get(resource, 0)}); update the device '
                                   'plugin shared-device count or release existing Pods')
    write_json(run.output / 'nodes.json', {n: nodes[n] for n in addresses})
    return addresses


def ensure_binary(run, client_image, require_mooncake=True):
    binary = getattr(run, 'binary_path', ROOT / 'output' / 'kvtest')
    if not binary.is_file():
        raise RuntimeError('missing output/kvtest; build with bash build.sh -b cmake --with-mooncake')
    cache = ROOT / 'build' / 'CMakeCache.txt'
    if require_mooncake and binary == ROOT / 'output' / 'kvtest' and (not cache.is_file() or
                            'KVTEST_ENABLE_MOONCAKE:BOOL=ON' not in cache.read_text(errors='replace')):
        raise RuntimeError('KVTest must be built with --with-mooncake')
    run.command(['nerdctl', '-n', 'k8s.io', 'image', 'inspect', client_image], timeout=30)
    run.command(['nerdctl', '-n', 'k8s.io', 'run', '--rm', '--net=none',
                 '-v', str(binary.parent) + ':/kvout:ro', '--entrypoint', 'sh', client_image,
                 '-ec', 'export LD_LIBRARY_PATH=/kvout/lib:$LD_LIBRARY_PATH; '
                 f'ldd {shlex.quote("/kvout/" + binary.name)} > /tmp/kvtest-ldd && cat /tmp/kvtest-ldd && '
                 '! grep -q "not found" /tmp/kvtest-ldd'], timeout=120)


def backend_binary(scenario, backend):
    artifacts = scenario.get('artifacts', {})
    path = artifacts.get('kvtest_binaries', {}).get(backend, artifacts.get('kvtest_binary', 'output/kvtest'))
    return (ROOT / path).resolve()


def ensure_mooncake_rdma_image(run, image):
    """Reject the UB build and missing service artifacts before creating Pods."""
    script = '\n'.join((
        'cache=/opt/Mooncake/build-ub/CMakeCache.txt',
        'test -r "$cache"',
        'grep "^USE_UB:BOOL=" "$cache"',
        'grep -qx "USE_UB:BOOL=OFF" "$cache"',
        'test -x /opt/Mooncake/build-ub/mooncake-store/src/mooncake_master',
        'test -f /opt/mooncake-bench/store_pod_target.py',
        'test -f /opt/Mooncake/build-ub/mooncake-store/src/libmooncake_store.a',
        'ldd /opt/Mooncake/build-ub/mooncake-store/src/mooncake_master > /tmp/kvtest-master-ldd',
        '! grep -q "not found" /tmp/kvtest-master-ldd',
    ))
    run.command(['nerdctl', '-n', 'k8s.io', 'run', '--rm', '--net=none',
                 '--entrypoint', 'sh', image, '-ec', script], timeout=120)


def ds_configs(run, scenario, coordinator_ip, ds_image):
    cfg_dir = run.output / 'image-config'
    cfg_dir.mkdir()
    if any(not ds_template_args(scenario, role) for role in ('coordinator', 'worker')):
        run.command(['nerdctl', '-n', 'k8s.io', 'run', '--rm', '--net=none', '--user', '0',
                     '-v', str(run.output) + ':/out', '--entrypoint', 'dscli', ds_image,
                     'generate_config', '-o', '/out/image-config'], timeout=120)
    address = coordinator_ip if ':' in coordinator_ip else coordinator_ip + ':31511'
    for role in ('coordinator', 'worker'):
        if ds_template_args(scenario, role):
            source, rendered = strict_ds_config(scenario, role, address)
            (cfg_dir / f'{role}-reference.json').write_bytes(source.encode('utf-8'))
            (cfg_dir / f'{role}.json').write_bytes(rendered.encode('utf-8'))
            continue
        file = cfg_dir / f'{role}_config.json'
        template = json.loads(file.read_text())
        if role == 'worker':
            reference = worker_reference(scenario)
            if reference is not None:
                template.update(reference)
                write_json(cfg_dir / 'worker-reference.json', reference)
        updates = dict(scenario['components']['datasystem'][role]['config'])
        updates['coordinator_address'] = address
        if role == 'worker':
            updates['cluster_name'] = scenario['cluster_name']
            updates['shared_memory_size_mb'] = scenario['memory_gib']['datasystem_worker'] * 1024
            updates['enable_urma'] = True
        patch_ds_config(template, updates)
        write_json(cfg_dir / f'{role}.json', template)
    return cfg_dir


def set_coordinator_addresses(config_dir, address):
    for role in ('coordinator', 'worker'):
        path = config_dir / f'{role}.json'
        config = json.loads(path.read_text(encoding='utf-8'))
        config['coordinator_address']['value'] = address
        source = path.read_bytes().decode('utf-8')
        path.write_bytes(render_ds_config(source, config, {'coordinator_address'}).encode('utf-8'))


def collect_pod_snapshot(run, namespace, case_dir):
    snapshot = run.kube_json('-n', namespace, 'get', 'pods')
    snapshot['snapshot_collected_at'] = datetime.now(timezone.utc).isoformat()
    write_json(case_dir / 'pods.json', snapshot)


def client_gen_command(plan, backend, namespace, prefix, case_dir,
                       service_address, scenario):
    cmd = [sys.executable, ROOT / 'deploy_client.py', 'gen-config', '-n', namespace,
           '-p', prefix, '--backend', backend, '--mode', 'pipeline',
           '--writer-count', str(plan.total_clients),
           '--pipeline', 'put' if backend == 'mooncake' else 'setStringView',
           '--notify-pipeline', ','.join(['get_buffer' if backend == 'mooncake' else 'getBuffer'] * plan.reads_per_notify),
           '--notify-count', str(plan.fanout),
           '--target-qps', str(plan.qps), '--data-sizes', plan.size,
           '--num-threads', str(plan.writer_threads),
           '--num-total-threads', str(plan.writer_threads + plan.read_threads), '--batch-keys-count', '1',
           '--cleanup-method', 'del', '--ttl', str(scenario.get('run', {}).get('ttl_seconds', 10)),
           '--cluster-name', scenario['cluster_name'],
           '--remote-work-dir', '/tmp/kvtest-cpp',
           '--output-dir', case_dir]
    numa = scenario.get('client_numa_node')
    if numa is None:
        cmd.append('--random-numa-node')
    else:
        cmd.extend(['--numa-node', str(numa), '--no-random-numa-node'])
    if backend == 'datasystem':
        client = scenario['components'][backend]['client'].get('config', {})
        cmd.extend(['--enable-local-cache', str(client.get('enable_local_cache', False)).lower()])
        jf = jf_settings(scenario, plan)
        if jf:
            cmd.extend(['--jf', service_address, '--service', jf['service']])
        else:
            cmd.extend(['--coordinator-address', service_address])
    else:
        cmd.extend(['--mooncake-master', service_address, '--mooncake-transport', 'rdma',
                    '--mooncake-local-buffer', str(scenario['memory_gib']['mooncake_client_buffer']) + 'GB'])
    return cmd


def configure_clients(run, scenario, plan, namespace, prefix, backend, case_dir,
                      service_address, duration, devices=None):
    env = component_environment(scenario, backend, 'client')
    cmd = client_gen_command(plan, backend, namespace, prefix, case_dir,
                             service_address, scenario)
    run.command(cmd, timeout=60)
    pods = run.pods(namespace, prefix, plan.total_clients)
    hosts = {name: pod['status']['hostIP'] for name, pod in pods.items()}
    deploy_path, config_path = case_dir / 'deploy.json', case_dir / 'config.json'
    deploy = json.loads(deploy_path.read_text())
    config = json.loads(config_path.read_text())
    patch_client_deploy(deploy, hosts, devices, duration)
    if plan.notify_include_self:
        peers = [f"http://{pods[node['pod_name']]['status']['podIP']}:"
                 f"{node.get('port', config.get('listen_port', 9000))}" for node in deploy['nodes']]
        if len(peers) != plan.total_clients or len(set(peers)) != plan.total_clients:
            raise RuntimeError('self notifications require one distinct endpoint per client')
        for node in deploy['nodes']:
            node['peers'] = list(peers)
    deploy['kvtest_binary_path'] = str(vars(run).get('binary_path', ROOT / 'output' / 'kvtest'))
    write_json(deploy_path, deploy)
    config['notify_include_self'] = plan.notify_include_self
    config['reads_per_notify'] = plan.reads_per_notify
    config['env'] = dict(config.get('env', {}), **env)
    write_json(case_dir / 'execution.json', {'duration_seconds': duration, 'timing': 'rc44 controller wall time after deploy'})
    if backend == 'datasystem':
        config['connect_options']['request_timeout_ms'] = 30000
        client = scenario['components'][backend]['client'].get('config', {})
        config['host_id_env_name'] = client.get('host_id_env_name', 'HOST_IP')
        for key in ('enable_local_cache', 'enable_cross_node_connection', 'connect_timeout_ms', 'request_timeout_ms',
                    'data_placement_policy', 'fast_transport_mem_size'):
            if key in client:
                config['connect_options'][key] = client[key]
        if 'request_timeout_seconds' in client:
            config['connect_options']['request_timeout_ms'] = client['request_timeout_seconds'] * 1000
    write_json(config_path, config)
    return deploy_path, config_path


def parse_summary(content):
    if re.search(r'Verify Fail: [1-9][0-9]*', content):
        raise ValueError('content verification failed')
    parsed = {}
    if re.search(r'--- put ---\s*Total: [1-9]', content):
        operations = ('put', 'get_buffer')
    elif re.search(r'--- setStringView ---\s*Total: [1-9]', content):
        operations = ('setStringView', 'getBuffer')
    else:
        operations = ('createBuffer', 'memoryCopy', 'setBuffer', 'getBuffer')
    for op in operations:
        match = re.search(r'--- ' + op + r' ---\s*Total: (\d+), Success: (\d+), Fail: (\d+)(.*?)(?=\n--- |\Z)',
                          content, flags=re.S)
        if not match:
            raise ValueError(f'missing {op} operation summary')
        total, success, failures = map(int, match.group(1, 2, 3))
        if success <= 0:
            raise ValueError(f'zero successful {op} operations')
        if failures or total != success:
            raise ValueError(f'{op} has {failures} failures')
        p99 = re.search(r'P99: ([0-9.]+)ms', match.group(4))
        avg = re.search(r'Avg: ([0-9.]+)ms', match.group(4))
        qps = re.search(r'QPS: ([0-9.]+)', match.group(4))
        if not all((p99, avg, qps)):
            raise ValueError(f'{op} has incomplete latency/QPS data')
        parsed[op] = {'total': total, 'success': success, 'fail': failures,
                      'avg_ms': float(avg.group(1)), 'p99_ms': float(p99.group(1)),
                      'qps': float(qps.group(1))}
    return parsed


def check_fanout(metrics, fanout):
    writes = sum(item[api_operations(item)[0]]['success'] for item in metrics.values())
    reads = sum(item[api_operations(item)[1]]['success'] for item in metrics.values())
    if reads != writes * fanout:
        raise ValueError(f'read fanout incomplete: {reads} successful reads for '
                         f'{writes} successful writes, expected {writes * fanout}')


API_LATENCY_PERCENTILES = (50, 90, 95, 99, 99.9, 99.99)
API_LATENCY_THRESHOLDS_US = (2000, 5000, 6000, 7000, 8000, 9000, 10000, 20000)


def aggregate_api_latency_samples(summaries):
    """DS uses native SDK access logs; only MC records external SDK-call samples."""
    operations = api_operations(next(iter(summaries.values())))
    is_mooncake = operations[0] == 'put'
    combined = {op: [] for op in operations}
    failures = {op: 0 for op in operations}
    evidence = {'source': 'mooncake_sdk_call' if is_mooncake else 'datasystem_sdk_access_log',
                'unit': 'microseconds', 'clients': []}
    for summary, metrics in summaries.items():
        if api_operations(metrics) != operations:
            raise RuntimeError('mixed SDK operation names across clients')
        counts = {op: 0 for op in operations}
        failed = {op: 0 for op in operations}
        files = []
        def record(op, latency, code):
            if latency < 0 or code < 0:
                raise RuntimeError(f'{summary}: negative latency or status code')
            combined[op].append(latency)
            counts[op] += 1
            failed[op] += int(code != 0)
        if is_mooncake:
            samples = summary.with_name('api_latency_samples.csv')
            status_file = summary.with_name('api_latency_samples.status.txt')
            if not status_file.is_file():
                raise RuntimeError(f'{status_file}: missing export status, process did not flush cleanly')
            status = json.loads(status_file.read_text())
            if status.get('io_error') is not False or status.get('dropped_samples') != 0:
                raise RuntimeError(f'{status_file}: incomplete latency export: {status}')
            files.append(str(samples))
            with samples.open(newline='', encoding='utf-8') as source:
                reader = csv.DictReader(source)
                if reader.fieldnames != ['op', 'latency_us', 'status_code']:
                    raise RuntimeError(f'{samples}: invalid CSV header')
                for row in reader:
                    if row['op'] not in combined:
                        raise RuntimeError(f'{samples}: unexpected operation {row["op"]!r}')
                    record(row['op'], int(row['latency_us']), int(row['status_code']))
            if status.get('written_samples') != sum(counts.values()):
                raise RuntimeError(f'{status_file}: written sample count differs from CSV')
        else:
            client = next((p for p in summary.parents if p.name.startswith('ds-client-')), None)
            if client is None:
                raise RuntimeError(f'{summary}: cannot identify client access-log directory')
            access = re.compile(r'(?:^|\|)\s*(\d+)\s*\|\s*(DS_KV_CLIENT_SET|DS_KV_CLIENT_GET)\s*\|\s*(\d+)\s*\|')
            names = {'DS_KV_CLIENT_SET': operations[0], 'DS_KV_CLIENT_GET': operations[1]}
            paths = sorted({p.resolve() for p in client.rglob('*access*.log*') if p.is_file()})
            for path in paths:
                files.append(str(path))
                opener = gzip.open if path.suffix == '.gz' else open
                with opener(path, 'rt', encoding='utf-8', errors='replace') as source:
                    for line in source:
                        match = access.search(line)
                        if match and 'kv_test_' in line:
                            record(names[match[2]], int(match[3]), int(match[1]))
        for op in operations:
            expected = metrics[op]
            if counts[op] > expected['total'] or (is_mooncake and (
                    counts[op] != expected['total'] or failed[op] != expected['fail'])):
                raise RuntimeError(f'{summary}: {op} sample count/fail {counts[op]}/{failed[op]} '
                                   f'differs from operation summary {expected}')
            if counts[op] == 0:
                raise RuntimeError(f'{summary}: no {op} latency samples; check SDK access logging/sampling')
            failures[op] += failed[op]
        evidence['clients'].append({'summary': str(summary), 'files': files, 'samples': counts,
            'operations': {op: metrics[op]['total'] for op in operations},
            'coverage': {op: counts[op] / metrics[op]['total'] for op in operations}})
    first = next(iter(summaries))
    collected = next((p for p in first.parents if p.name == 'collected'), None)
    write_json((collected.parent if collected else first.parent) / 'latency-source.json', evidence)

    result = {}
    for op in operations:
        values = sorted(combined[op])
        if not values:
            raise RuntimeError(f'{op}: no raw API latency samples')
        n = len(values)
        stats = {'total': n, 'fail': failures[op], 'fail_rate_pct': failures[op] * 100.0 / n,
                 'min_us': values[0], 'avg_us': sum(values) / n, 'max_us': values[-1]}
        # Match access_log_status.py: interpolate at n * percentile / 100.
        for percentile in API_LATENCY_PERCENTILES:
            position = n * percentile / 100.0
            lower = int(position)
            upper = min(lower + 1, n - 1)
            stats[f'p{str(percentile).replace(".", "")}_us'] = (
                values[lower] + (position - lower) * (values[upper] - values[lower]))
        for threshold in API_LATENCY_THRESHOLDS_US:
            stats[f'gt{threshold}'] = sum(value > threshold for value in values)
        result[op] = stats
    return result


def format_api_latency_table(stats, backend):
    header = ('API                         Total   Fail  FailRate     Min     Avg     P50'
              '     P90     P95     P99    P99.9   P99.99      Max'
              '  >2000us  >5000us  >6000us  >7000us  >8000us  >9000us  >10000us  >20000us')
    source = 'SDK access-log' if backend == 'datasystem' else 'KVTest SDK-call'
    rows = [f'{backend} {source} latency (microseconds, all clients)', header,
            '-' * len(header)]
    for op in api_operations(stats):
        item = stats[op]
        name = ('DS_' if backend == 'datasystem' else 'MC_') + api_label(backend, op)
        values = [item[key] for key in ('min_us', 'avg_us', 'p50_us', 'p90_us',
                                         'p95_us', 'p99_us', 'p999_us', 'p9999_us', 'max_us')]
        thresholds = [item[f'gt{threshold}'] for threshold in API_LATENCY_THRESHOLDS_US]
        rows.append(f'{name:<26} {item["total"]:>7} {item["fail"]:>6} '
                    f'{item["fail_rate_pct"]:>8.2f}% '
                    + ' '.join(f'{value:>7.0f}' for value in values)
                    + '  ' + ' '.join(f'{count:>8}' for count in thresholds))
    return '\n'.join(rows)


def write_api_latency_stats(case_dir, stats):
    if case_dir.name == 'mooncake':
        stats = {api_label('mooncake', op): item for op, item in stats.items()}
    write_json(case_dir / 'api_latency_summary.json', stats)
    table = format_api_latency_table(stats, case_dir.name)
    (case_dir / 'api_latency_summary.txt').write_text(table + '\n', encoding='utf-8')
    fields = ['api', 'total', 'fail', 'fail_rate_pct', 'min_us', 'avg_us',
              'p50_us', 'p90_us', 'p95_us', 'p99_us', 'p999_us', 'p9999_us', 'max_us']
    fields += [f'gt{threshold}' for threshold in API_LATENCY_THRESHOLDS_US]
    with (case_dir / 'api_latency_summary.csv').open('w', newline='', encoding='utf-8') as target:
        writer = csv.DictWriter(target, fieldnames=fields)
        writer.writeheader()
        for op in api_operations(stats):
            writer.writerow({'api': api_label(case_dir.name, op), **stats[op]})
    return table


def collect_and_check(case_dir, plan):
    summaries = sorted((case_dir / 'collected').rglob('run_summary.txt'))
    if len(summaries) != plan.total_clients:
        raise RuntimeError(f'{case_dir.name}: expected {plan.total_clients} client summaries, found {len(summaries)}')
    metrics = {}
    parsed_by_path = {}
    for path in summaries:
        try:
            parsed = parse_summary(path.read_text(errors='replace'))
            metrics[str(path.relative_to(case_dir))] = parsed
            parsed_by_path[path] = parsed
        except ValueError as exc:
            raise RuntimeError(f'{path}: {exc}') from exc
    stats = aggregate_api_latency_samples(parsed_by_path)
    table = write_api_latency_stats(case_dir, stats)
    try:
        check_fanout(metrics, plan.reads_per_write)
    except ValueError as exc:
        print(f'{case_dir.name}: WARNING: {exc}; continuing with observed counts', flush=True)
    writes = sum(item[api_operations(item)[0]]['success'] for item in metrics.values())
    reads = sum(item[api_operations(item)[1]]['success'] for item in metrics.values())
    write_json(case_dir / 'fanout.json', {'successful_set': writes, 'successful_get': reads,
               'configured_fanout': plan.reads_per_write, 'notify_count': plan.fanout,
               'effective_notify_count': plan.effective_fanout,
               'reads_per_notify': plan.reads_per_notify, 'observed_get_per_set': reads / writes if writes else 0})
    write_name, read_name = ('put', 'get_buffer') if case_dir.name == 'mooncake' else ('Set', 'Get')
    print(f'{case_dir.name}: {write_name}={writes}, {read_name}={reads}, '
          f'observed {read_name}/{write_name}={reads / writes:.3f}', flush=True)
    print(table, flush=True)
    write_json(case_dir / 'metrics.json', metrics)
    return summaries


def render_report(output, plan, backends=('datasystem', 'mooncake')):
    lines = [
        f'# KVTest {plan.size} 单轮观测', '',
        f'本次系统：{", ".join(backends)}，各运行一次。client 分布 {dict(plan.placement(backends[0], "client"))}，'
        f'每 client {plan.qps} 目标写 QPS、{plan.writer_threads} 写线程、{plan.read_threads} 读线程，'
        f'通知上限 {plan.fanout} 个 client、有效通知 {plan.effective_fanout} 个 client，'
        f'每个通知执行 {plan.reads_per_notify} 次读，目标读/写 {plan.reads_per_write}。', '',
        f'通知候选包含写入者自己：{"是" if plan.notify_include_self else "否"}。', '',
        'DataSystem 写入/读取接口以本轮配置和摘要为准；Mooncake 运行 `put → get_buffer`。'
        'DataSystem 使用 rc44 SDK 原生 access 日志，Mooncake 使用独立操作层的 SDK 调用外侧打点。'
        '使用与 access_log_status.py 相同的分位数插值和微秒阈值统计。'
        '两种计时边界不同，DS 日志采样覆盖率另见 latency-source.json。', '',
    ]
    boundary_notes = []
    for backend in backends:
        aggregate = json.loads((output / backend / 'api_latency_summary.json').read_text())
        boundary_notes.append(api_boundary_note(backend, aggregate))
        fanout = json.loads((output / backend / 'fanout.json').read_text())
        write_name, read_name = [api_label(backend, op) for op in api_operations(aggregate)]
        lines.extend([f'## {backend} 全部 client 接口耗时', '',
                      f"实际成功 {write_name}={fanout['successful_set']}，{read_name}={fanout['successful_get']}，"
                         f"{read_name}/{write_name}={fanout['observed_get_per_set']:.3f}（配置 {plan.reads_per_write}）。读取数量不作严格相等校验。", '',
                      '| 接口 | Total | Fail | Avg (us) | P50 (us) | P90 (us) | P95 (us) | P99 (us) | P99.9 (us) | P99.99 (us) |',
                      '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |'])
        for op in api_operations(aggregate):
            item = aggregate[op]
            lines.append(f"| {api_label(backend, op)} | {item['total']} | {item['fail']} | {item['avg_us']:.0f} | "
                         f"{item['p50_us']:.0f} | {item['p90_us']:.0f} | {item['p95_us']:.0f} | "
                         f"{item['p99_us']:.0f} | {item['p999_us']:.0f} | {item['p9999_us']:.0f} |")
        lines.extend(['', f'完整阈值计数见 `{backend}/api_latency_summary.csv`。', ''])
    lines.extend([
        '## 各 client 指标', '',
        '下表是各 client 自身的操作统计；'
        'P99 是各 client 的原始值，没有跨 client 合并。', '',
        '| 系统 | client | 写接口 | 读接口 | 写成功 | 写 QPS | 写 Avg (ms) | 写 P99 (ms) | 读成功 | 读 QPS | 读 Avg (ms) | 读 P99 (ms) |',
        '| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |',
    ])
    for backend in backends:
        metrics = json.loads((output / backend / 'metrics.json').read_text())
        for path, item in sorted(metrics.items()):
            write_op, read_op = api_operations(item)
            write, read = item[write_op], item[read_op]
            client_label = Path(path).parts[1]
            lines.append(f"| {backend} | {client_label} | {api_label(backend, write_op)} | "
                         f"{api_label(backend, read_op)} | {write['success']} | {write['qps']:.2f} | "
                         f"{write['avg_ms']:.3f} | {write['p99_ms']:.3f} | {read['success']} | "
                         f"{read['qps']:.2f} | {read['avg_ms']:.3f} | {read['p99_ms']:.3f} |")
    lines.extend(['', '这是一轮观测，没有轮间波动证据；目标 QPS 不等于实测 QPS。',
                  ''.join(boundary_notes) + '两端底层数据动作和'
                  '缓存淘汰语义不同，本报告不据此宣称同口径性能胜负。仅执行大小校验。',
                  '原始配置、Pod 镜像与逐 client 日志见本目录。', ''])
    (output / 'report.md').write_text('\n'.join(lines), encoding='utf-8')
    from kvtest_report import render_html_report
    render_html_report(output, backends, {'size': plan.size, 'qps': plan.qps, 'writer_threads': plan.writer_threads,
                                        'fanout': plan.fanout, 'effective_notify_count': plan.effective_fanout,
                                        'reads_per_notify': plan.reads_per_notify, 'reads_per_write': plan.reads_per_write})


def query_client_stats(address):
    # Pod addresses must bypass host HTTP proxy settings.
    with build_opener(ProxyHandler({})).open(f'http://{address}:9000/stats', timeout=5) as response:
        result = json.load(response)
    if 'stats_json' in result:
        result = json.loads(result['stats_json'])
    return result


def wait_pipeline_writers(case_dir, plan, started_at=None):
    """rc44 runs until stopped externally; keep native writers/queues unchanged."""
    duration = json.loads((case_dir / 'execution.json').read_text())['duration_seconds']
    started_at = time.monotonic() if started_at is None else started_at
    remaining = duration - (time.monotonic() - started_at)
    print(f'rc44 native pipeline: {duration}s controller window after client deployment', flush=True)
    while remaining > 0:
        time.sleep(min(1, remaining))
        remaining = duration - (time.monotonic() - started_at)
    write_json(case_dir / 'drain-status.json', {
        'mode': 'rc44 external stop; no writer drain API',
        'duration_seconds': duration, 'elapsed_after_deploy_seconds': time.monotonic() - started_at})


def run_clients(run, namespace, case_dir, plan):
    run.command([sys.executable, ROOT / 'deploy_client.py', 'deploy',
                 case_dir / 'deploy.json', case_dir / 'config.json'], cwd=case_dir, timeout=900)
    wait_pipeline_writers(case_dir, plan)
    run.command([sys.executable, ROOT / 'deploy_client.py', 'stop',
                 case_dir / 'deploy.json', case_dir / 'config.json', '--stop-timeout', '60'], cwd=case_dir, timeout=180)
    run.command([sys.executable, ROOT / 'deploy_client.py', 'collect',
                 case_dir / 'deploy.json', case_dir / 'config.json', '--summary-timeout', '0',
                 '-o', case_dir / 'collected'],
                cwd=case_dir, timeout=300)
    return collect_and_check(case_dir, plan)


def collect_startup_snapshot(run, namespace, case_dir, prefixes=None):
    probe = (ROOT / 'kvtest_service_snapshot.py').read_text(encoding='utf-8')
    pods = json.loads((case_dir / 'pods.json').read_text())['items']
    evidence = {'collected_at': datetime.now(timezone.utc).isoformat(), 'pods': []}
    for pod in pods:
        if prefixes and not pod['metadata']['name'].startswith(prefixes):
            continue
        record = {'pod': pod['metadata']['name'], 'node': pod['spec'].get('nodeName', '')}
        try:
            result = run.command(['kubectl', '-n', namespace, 'exec', record['pod'], '--',
                                  'python3', '-c', probe], capture=True, timeout=15,
                                 display=f"kubectl -n {namespace} exec {record['pod']} -- python3 [procfs snapshot]")
            record['processes'] = json.loads(result)
            if not record['processes']:
                record['capture_error'] = 'No matching live service processes found'
        except (RuntimeError, ValueError, subprocess.TimeoutExpired, OSError) as error:
            record['capture_error'] = str(error)
            print(f"Startup snapshot unavailable for {record['pod']}: {error}", flush=True)
        evidence['pods'].append(record)
    write_json(case_dir / 'service-startup.json', evidence)


def collect_datasystem_services(run, namespace, case_dir):
    path = case_dir / 'service-discovery.json'
    discovery = json.loads(path.read_text()) if path.is_file() else {'mode': 'dscli'}
    standalone = discovery['mode'] == 'standalone_jf'
    for role in ('coordinator', 'worker'):
        remote_dir = f'/data/standalone/{role}' if standalone else '/data'
        run.command([sys.executable, ROOT / f'deploy_{role}.py', 'collect', '-n', namespace,
                     '-p', f'ds-{role}-', '--remote-config', f'/data/{role}.json',
                     '--remote-dir', remote_dir, '-o', case_dir / f'{role}-logs'], timeout=180)
    if standalone:
        run.command([sys.executable, ROOT / 'deploy_jf.py', 'collect', '-n', namespace,
                     '-p', 'ds-jf-', '--remote-dir', '/data/jf', '-o', case_dir / 'jf-logs'], timeout=120)


def execute_trial(scenario, args, output):
    plan = plan_from_scenario(scenario)
    backends = configured_systems(scenario)
    mc_image = args.mc_image
    pod_images = {backend: scenario.get('artifacts', {}).get('pod_images', {}).get(backend, mc_image)
                  for backend in backends}
    if output.exists():
        raise ValueError(f'output already exists: {output}')
    output.mkdir(parents=True)
    run = Run(output)
    run.binary_path = args.binary_path
    write_json(output / 'scenario-source.json', args.source_scenario)
    write_json(output / 'scenario-input.json', scenario)
    effective = dict(scenario)
    effective['sizes'] = [plan.size]
    effective['target_qps'] = {plan.size: [plan.qps]}
    effective['notify_include_self'] = plan.notify_include_self
    effective.pop('writes_per_client', None)
    effective['execution'] = {'duration_seconds': args.duration, 'rounds_per_system': 1,
                              'systems': list(backends),
                              'ttl_seconds': scenario.get('run', {}).get('ttl_seconds', 10),
                              'cluster_name': scenario['cluster_name'], 'verify_level': 'size',
                              'write_limit': 'rc44 external wall-time stop'}
    effective['execution']['pipelines_by_system'] = {
        'datasystem': {'writer': ['setStringView'], 'reader': ['getBuffer'] * plan.reads_per_notify},
        'mooncake': {'writer': ['put'], 'reader': ['get_buffer'] * plan.reads_per_notify}}
    if 'mooncake' in backends and scenario['components']['mooncake']['client'].get('config'):
        effective['execution']['legacy_ignored_fields'] = [
            'components.mooncake.client.config.request_timeout_seconds (not supported by Mooncake SDK adapter)']
    write_json(output / 'scenario-effective.json', effective)
    write_json(output / 'images.json', {'datasystem': pod_images.get('datasystem'),
                                       'mooncake': pod_images.get('mooncake'),
                                       'pod_images': pod_images,
                                       'datasystem_config_image': args.ds_image,
                                       'datasystem_whl': str(args.ds_whl) if args.ds_whl else None})
    state = {'status': 'running', 'cases': [], 'error': None}
    write_json(output / 'state.json', state)
    names = []
    try:
        print('[1/7] Preflight')
        addresses = node_addresses(run, plan, scenario, backends)
        run.addresses = addresses
        for backend, image in pod_images.items():
            run.binary_path = backend_binary(scenario, backend)
            ensure_binary(run, image, require_mooncake=backend == 'mooncake')
            if backend == 'datasystem':
                if jf_settings(scenario, plan):
                    check_native_package(scenario)
                elif not args.ds_whl:
                    raise ValueError('dscli deployment requires --ds-whl or artifacts.datasystem_whl')
        if 'mooncake' in backends:
            ensure_mooncake_rdma_image(run, pod_images['mooncake'])
        config_dir = ds_configs(run, scenario, '127.0.0.1:31511', args.ds_image) if 'datasystem' in backends else None
        templates = output / 'templates'
        templates.mkdir()
        template_roles = [('control', None)] + [(backend, scenario['ub_resource'] if backend == 'datasystem'
                                               else scenario['mooncake_rdma']['resource']) for backend in backends]
        for role, resource in template_roles:
            (templates / f'{role}.yaml').write_text(
                pod_template(resource, ipc_lock=role == 'mooncake'), encoding='utf-8')
        tag = uuid.uuid4().hex[:7]
        specs = [(backend, f'kvtest-{backend[:2]}-{tag}', pod_images[backend])
                 for backend in backends]
        for backend, namespace, image in specs:
            run.binary_path = backend_binary(scenario, backend)
            names.append(namespace)
            case_dir = output / backend
            case_dir.mkdir()
            print(f'\n[2/7] Create isolated namespace {namespace}')
            run.command(['kubectl', 'create', 'namespace', namespace], timeout=30)
            memory = scenario['memory_gib']
            if backend == 'datasystem':
                print('[3/7] Deploy Coordinator and worker Pods')
                jf = jf_settings(scenario, plan)
                discovery = (start_jf_mock(run, scenario, plan, namespace, image, templates / 'control.yaml')
                             if jf else {'mode': 'dscli'})
                write_json(case_dir / 'service-discovery.json', discovery)
                jf_address = discovery.get('jf_address')
                coordinator = deploy_component(run, scenario, plan, namespace, backend, 'coordinator', image, templates)
                service = ','.join(pod['status']['podIP'] + ':31511' for pod in coordinator.values())
                set_coordinator_addresses(config_dir, service)
                run.command([sys.executable, ROOT / 'deploy_coordinator.py', 'deploy', '-n', namespace,
                             '-p', 'ds-coordinator-', '-c', config_dir / 'coordinator.json',
                             '--remote-config', '/data/coordinator.json', '--port', '31511',
                             *ds_template_args(scenario, 'coordinator'),
                             *datasystem_service_args(scenario, plan, 'coordinator', jf_address, args.ds_whl),
                             '--no-procmon'], timeout=600)
                if jf:
                    discovery['registration'] = wait_jf_registration(run, namespace, discovery, service.split(','))
                    write_json(case_dir / 'service-discovery.json', discovery)
                deploy_component(run, scenario, plan, namespace, backend, 'worker', image, templates)
                print('[4/7] Deploy client Pods and start DataSystem workers')
                deploy_component(run, scenario, plan, namespace, backend, 'client', image, templates)
                run.command([sys.executable, ROOT / 'deploy_worker.py', 'deploy', '-n', namespace,
                             '-p', 'ds-worker-', '-c', config_dir / 'worker.json',
                             '--remote-config', '/data/worker.json', '--port', '31501',
                             *ds_template_args(scenario, 'worker'),
                             *datasystem_service_args(scenario, plan, 'worker', jf_address, args.ds_whl),
                             '--no-procmon'], timeout=600)
                if jf:
                    service = jf_address
                prefix = 'ds-client-'
                devices = None
            else:
                print('[3/7] Deploy Mooncake master and store Pods')
                master = deploy_component(run, scenario, plan, namespace, backend, 'master', image, templates)
                port = scenario['components']['mooncake']['master'].get('args', {}).get('rpc_port', 50051)
                service = next(iter(master.values()))['status']['podIP'] + f':{port}'
                run.command([sys.executable, ROOT / 'deploy_mooncake.py', 'start', '--role', 'master',
                             '-n', namespace, '-p', 'mc-master-', '--port', str(port), '--timeout', '120',
                             *mooncake_master_args(scenario, case_dir)], timeout=180)
                stores = deploy_component(run, scenario, plan, namespace, backend, 'store', image, templates)
                print('[4/7] Deploy Mooncake client Pods and resolve allocated HCAs')
                clients = deploy_component(run, scenario, plan, namespace, backend, 'client', image, templates)
                devices = {}
                for name in [*stores, *clients]:
                    run.command(['kubectl', '-n', namespace, 'cp',
                                 str(ROOT / 'tools' / 'mooncake' / 'rdma_device.py'),
                                 name + ':/tmp/rdma_device.py'], timeout=60)
                    selected = run.command(['kubectl', '-n', namespace, 'exec', name, '--',
                                            'python3', '/tmp/rdma_device.py', '--device-name=' + rdma_device_preference(scenario, (stores | clients)[name]),
                                            '--require-ipc-lock'],
                                           capture=True, timeout=30)
                    devices[name] = selected.strip().splitlines()[-1]
                write_json(case_dir / 'rdma-devices.json', devices)
                run.command([sys.executable, ROOT / 'deploy_mooncake.py', 'start', '--role', 'store',
                             '-n', namespace, '-p', 'mc-store-', '--master', service,
                             '--protocol', 'rdma', '--segment-size',
                             str(memory['mooncake_store'] * 1024**3), '--local-buffer-size',
                             str(memory['mooncake_client_buffer'] * 1024**3), '--devices-json',
                             case_dir / 'rdma-devices.json', '--timeout', '180',
                             *mooncake_store_env_args(scenario, case_dir)], timeout=240)
                prefix = 'mc-client-'
            print('[5/7] Generate KVTest client configs')
            time.sleep(scenario.get('run', {}).get('service_settle_seconds', 20))
            configure_clients(run, scenario, plan, namespace, prefix, backend,
                              case_dir, service, args.duration, devices)
            collect_pod_snapshot(run, namespace, case_dir)
            if backend == 'datasystem' and jf:
                collect_startup_snapshot(run, namespace, case_dir, ('ds-coordinator-', 'ds-worker-', 'ds-jf-'))
            print(f'[6/7] Run one {plan.size} case, then collect logs')
            summaries = run_clients(run, namespace, case_dir, plan)
            if backend == 'datasystem':
                collect_datasystem_services(run, namespace, case_dir)
            else:
                for role in ('master', 'store'):
                    run.command([sys.executable, ROOT / 'deploy_mooncake.py', 'collect', '--role', role,
                                 '-n', namespace, '-p', f'mc-{role}-', '-o', case_dir / f'{role}-logs'], timeout=180)
            state['cases'].append({'backend': backend, 'namespace': namespace,
                                   'summaries': [str(p.relative_to(output)) for p in summaries]})
            write_json(output / 'state.json', state)
            if scenario.get('run', {}).get('cleanup_on_success', True):
                print('[7/7] Release this case namespace')
                run.command(['kubectl', 'delete', 'namespace', namespace, '--wait=true', '--timeout=300s'], timeout=330)
                names.remove(namespace)
            else:
                print('[7/7] Retain namespace:', namespace)
        render_report(output, plan, backends)
        state['status'] = 'complete'
        state['retained_namespaces'] = names
        write_json(output / 'state.json', state)
        print('HTML report:', output / 'report.html')
        print('Results:', output)
        return 0
    except (RuntimeError, OSError, subprocess.TimeoutExpired, ValueError, KeyboardInterrupt) as exc:
        state['status'] = 'failed'
        state['error'] = str(exc)
        state['retained_namespaces'] = names
        write_json(output / 'state.json', state)
        print(f'Failed: {exc}\nResults: {output}\nRetained namespaces: {names}', file=sys.stderr)
        return 1


def resolve_run_settings(scenario, args):
    scenario = copy.deepcopy(scenario)
    settings = scenario.get('run', {})
    if not isinstance(settings, dict) or set(settings) - {
            'duration_seconds', 'cleanup_on_success', 'ttl_seconds', 'service_settle_seconds', 'output_dir', 'output_root'}:
        raise ValueError('run has unknown fields or is not an object')
    artifacts = scenario.get('artifacts', {})
    if not isinstance(artifacts, dict) or set(artifacts) - {
            'pod_image', 'pod_images', 'datasystem_config_image', 'datasystem_whl', 'kvtest_binary', 'kvtest_binaries'}:
        raise ValueError('artifacts has unknown fields or is not an object')
    binaries = artifacts.get('kvtest_binaries', {})
    if (not isinstance(binaries, dict) or set(binaries) - {'datasystem', 'mooncake'}
            or any(not isinstance(value, str) or not value.strip() for value in binaries.values())):
        raise ValueError('artifacts.kvtest_binaries must map system names to binary paths')
    for field, default in (('ttl_seconds', 10), ('service_settle_seconds', 20)):
        value = settings.get(field, default)
        if type(value) is not int or value < 0:
            raise ValueError(f'run.{field} must be a nonnegative integer')
    if type(settings.get('cleanup_on_success', True)) is not bool:
        raise ValueError('run.cleanup_on_success must be true or false')
    args.duration = args.duration if args.duration is not None else settings.get('duration_seconds', 60)
    if type(args.duration) is not int or args.duration <= 0:
        raise ValueError('duration must be a positive integer')
    scenario.setdefault('run', {})['duration_seconds'] = args.duration
    if args.systems is not None:
        scenario['systems'] = list(selected_backends(args.systems))
    configured_systems(scenario)
    if args.client_cpu_limit is not None:
        scenario['client_cpu_limit'] = args.client_cpu_limit
    if args.target_qps is not None:
        scenario['target_qps'] = {size: [args.target_qps] for size in scenario.get('sizes', [DEFAULT_DATA_SIZE])}
    source_image = ROOT / 'mooncake-output' / 'source_image.txt'
    args.mc_image = args.mc_image or artifacts.get('pod_image')
    if not args.mc_image and source_image.is_file():
        args.mc_image = source_image.read_text(encoding='utf-8').strip()
    images = artifacts.get('pod_images', {})
    if not isinstance(images, dict) or set(images) - {'datasystem', 'mooncake'}:
        raise ValueError('artifacts.pod_images must map system names to images')
    if not all(isinstance(images.get(backend, args.mc_image), str) and images.get(backend, args.mc_image).strip()
               for backend in configured_systems(scenario)):
        raise ValueError('configure artifacts.pod_image / pod_images or --mc-image')
    args.ds_image = args.ds_image or artifacts.get('datasystem_config_image', DS_IMAGE)
    args.ds_whl = args.ds_whl or artifacts.get('datasystem_whl')
    if args.ds_whl:
        args.ds_whl = (ROOT / args.ds_whl).resolve()
    args.binary_path = (ROOT / artifacts.get('kvtest_binary', 'output/kvtest')).resolve()
    scenario['artifacts'] = dict(artifacts, pod_image=args.mc_image, datasystem_config_image=args.ds_image,
                                 kvtest_binary=str(args.binary_path))
    if args.ds_whl:
        scenario['artifacts']['datasystem_whl'] = str(args.ds_whl)
    return scenario


def trial_description(scenario):
    plan = plan_from_scenario(scenario)
    jf = jf_settings(scenario, plan) if 'datasystem' in configured_systems(scenario) else None
    return {'size': plan.size, 'qps_per_client': plan.qps, 'writer_threads': plan.writer_threads,
            'reader_threads': plan.read_threads, 'notify_count': plan.fanout, 'total_clients': plan.total_clients,
            'effective_notify_count': plan.effective_fanout,
            'reads_per_notify': plan.reads_per_notify, 'reads_per_write': plan.reads_per_write,
            'notify_include_self': plan.notify_include_self,
            'datasystem_deployment': {'mode': 'standalone_jf', 'jf': jf} if jf else {'mode': 'dscli'},
            'components': {backend: {role: dict(plan.placement(backend, role))
                                     for role in (*(('coordinator', 'worker') if backend == 'datasystem' else ('master', 'store')), 'client')}
                           for backend in configured_systems(scenario)}}


def execute_matrix(scenarios, args, output):
    from kvtest_report import render_matrix_report
    if output.exists():
        raise ValueError(f'output already exists: {output}')
    output.mkdir(parents=True)
    state = {'status': 'running', 'trials': [], 'retained_namespaces': []}
    write_json(output / 'scenario-input.json', args.source_scenario)
    write_json(output / 'scenario-source.json', args.source_scenario)
    write_json(output / 'scenario-effective.json', args.effective_scenario)
    for index, scenario in enumerate(scenarios, start=1):
        plan = plan_from_scenario(scenario)
        relative = Path('trials') / f'{index:03d}-{plan.size.lower()}-qps{plan.qps}'
        record = dict(trial_description(scenario), directory=relative.as_posix(), status='running')
        state['trials'].append(record)
        write_json(output / 'state.json', state)
        result = execute_trial(scenario, args, output / relative)
        trial_state = json.loads((output / relative / 'state.json').read_text(encoding='utf-8'))
        record.update(status=trial_state['status'], error=trial_state.get('error'))
        state['retained_namespaces'].extend(trial_state.get('retained_namespaces', []))
        if result:
            state.update(status='failed', error=record['error'])
            write_json(output / 'state.json', state)
            render_matrix_report(output, state)
            return result
    state['status'] = 'complete'
    write_json(output / 'state.json', state)
    render_matrix_report(output, state)
    print('HTML report:', output / 'report.html')
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scenario', type=Path, default=SCENARIO)
    parser.add_argument('--ds-image', help='override the DataSystem config-template image')
    parser.add_argument('--ds-whl', type=Path, help='override artifacts.datasystem_whl')
    parser.add_argument('--mc-image', help='override the common Pod image')
    parser.add_argument('--systems', choices=('both', 'datasystem', 'mooncake'))
    parser.add_argument('--duration', type=int, help='override run.duration_seconds')
    parser.add_argument('--target-qps', type=int, help='override all configured size rates with one per-client write QPS')
    parser.add_argument('--client-cpu-limit', type=int)
    parser.add_argument('--output', type=Path, help='override run.output_dir')
    parser.add_argument('--validate-only', action='store_true', help='validate JSON and print every trial without cluster access')
    args = parser.parse_args(argv)
    try:
        args.source_scenario = json.loads(args.scenario.read_text(encoding='utf-8'))
        if not isinstance(args.source_scenario, dict):
            raise ValueError('scenario must be a JSON object')
        source = copy.deepcopy(args.source_scenario)
        if args.mc_image:
            args.source_scenario.setdefault('artifacts', {})['pod_images'] = {}
        scenario = resolve_run_settings(args.source_scenario, args)
        args.effective_scenario = scenario
        args.source_scenario = source
        scenarios = expand_scenarios(scenario)
        for trial in scenarios:
            plan = plan_from_scenario(trial)
            check_memory_budget(plan, trial, args.duration)
        if args.validate_only:
            print(json.dumps({'systems': configured_systems(scenario), 'duration_seconds': args.duration,
                              'trials': [trial_description(trial) for trial in scenarios]}, ensure_ascii=False, indent=2))
            return 0
        if ('datasystem' in configured_systems(scenario)
                and not jf_settings(scenario, plan_from_scenario(scenarios[0]))):
            if args.ds_whl is None:
                args.ds_whl = next(iter(sorted((ROOT / 'datasystem-output').glob('openyuanrong_datasystem-*.whl'), reverse=True)), None)
            if args.ds_whl is None or not args.ds_whl.is_file():
                raise ValueError('DataSystem whl missing: configure artifacts.datasystem_whl or --ds-whl')
            scenario['artifacts']['datasystem_whl'] = str(args.ds_whl)
            for trial in scenarios:
                trial['artifacts']['datasystem_whl'] = str(args.ds_whl)
        settings = scenario.get('run', {})
        configured_output = args.output or settings.get('output_dir')
        tag = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + uuid.uuid4().hex[:6]
        if configured_output:
            output = (ROOT / configured_output).resolve()
        else:
            prefix = f'kvtest-{scenario_size(scenarios[0]).lower()}' if len(scenarios) == 1 else 'kvtest-matrix'
            output = (ROOT / settings.get('output_root', 'results') / f'{prefix}-{tag}').resolve()
        if len(scenarios) == 1:
            return execute_trial(scenarios[0], args, output)
        return execute_matrix(scenarios, args, output)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    raise SystemExit(main())
