"""Logical host grouping for Kubernetes comparison deployments."""
import re


def pod_name(prefix, replica, node):
    return f'{prefix}-{replica}-{node.replace(".", "-").replace("_", "-")}'


def validate_host_id(value):
    if not isinstance(value, str) or not re.fullmatch(r'[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?', value):
        raise ValueError('logical host ID must be a lowercase identifier of 1-63 characters')
    return value


def logical_host_groups(scenario, plan, backend):
    enabled = scenario.get('logical_hosts', False)
    if type(enabled) is not bool:
        raise ValueError('logical_hosts must be boolean')
    if not enabled:
        return []
    role = 'worker' if backend == 'datasystem' else 'store'
    storage = dict(plan.placement(backend, role))
    clients = dict(plan.placement(backend, 'client'))
    if set(storage) != set(clients) or any(clients[node] != count * 2 for node, count in storage.items()):
        raise ValueError('logical_hosts requires one worker/store and two clients per logical host on each node')
    prefix = 'ds' if backend == 'datasystem' else 'mc'
    groups = []
    for node in sorted(storage):
        for replica in range(storage[node]):
            groups.append({
                'host_id': f'node-{len(groups) + 1:02d}', 'physical_node': node,
                'storage_pod': pod_name(f'{prefix}-{role}', replica, node),
                'client_pods': [pod_name(f'{prefix}-client', replica * 2 + offset, node) for offset in range(2)]})
    return groups


def logical_host_ids(scenario, plan, backend, role):
    groups = logical_host_groups(scenario, plan, backend)
    return {name: group['host_id'] for group in groups
            for name in (group['client_pods'] if role == 'client' else [group['storage_pod']])}
