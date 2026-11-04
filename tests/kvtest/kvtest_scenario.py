"""Validated workload and Kubernetes placement shared by KVTest entrypoints."""
import copy
from dataclasses import dataclass
import re


DEFAULT_DATA_SIZE = '8MB'
SIZE_UNITS = {'B': 1, 'KB': 1024, 'MB': 1024**2, 'GB': 1024**3}
BACKEND_ROLES = {'datasystem': ('coordinator', 'worker'), 'mooncake': ('master', 'store')}
COORDINATOR_COUNTS = (1, 3, 5)
NODE_NAME = re.compile(r'[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?')


@dataclass(frozen=True)
class Plan:
    nodes: tuple
    clients_per_node: int
    qps: int
    writer_threads: int
    fanout: int
    size: str = DEFAULT_DATA_SIZE
    reader_threads: int = 0
    clients: tuple = ()
    components: tuple = ()
    notify_include_self: bool = False
    reads_per_notify: int = 1

    @property
    def effective_fanout(self):
        eligible = self.total_clients if self.notify_include_self else self.total_clients - 1
        return min(self.fanout, eligible)

    @property
    def reads_per_write(self):
        return self.effective_fanout * self.reads_per_notify

    @property
    def total_clients(self):
        return sum(count for _, count in self.clients) if self.clients else len(self.nodes) * self.clients_per_node

    @property
    def read_threads(self):
        return self.reader_threads or self.writer_threads

    def placement(self, backend, role):
        if role == 'client':
            return self.clients or tuple((node, self.clients_per_node) for node in self.nodes)
        return next((nodes for system, component, nodes in self.components
                     if (system, component) == (backend, role)), tuple((node, 1) for node in self.nodes))


def positive_integer(value, label):
    if type(value) is not int or value <= 0:
        raise ValueError(f'{label} must be a positive integer')
    return value


def data_size_bytes(value):
    match = re.fullmatch(r'([1-9][0-9]*)(B|KB|MB|GB|b|kb|mb|gb)', value) if isinstance(value, str) else None
    if not match:
        raise ValueError('data size must be a positive integer followed by B, KB, MB or GB')
    return int(match.group(1)) * SIZE_UNITS[match.group(2).upper()]


def scenario_size(scenario):
    sizes = scenario.get('sizes', [DEFAULT_DATA_SIZE])
    if not isinstance(sizes, list) or len(sizes) != 1:
        raise ValueError('sizes must contain exactly one data size per trial')
    data_size_bytes(sizes[0])
    return sizes[0]


def configured_systems(scenario):
    systems = scenario.get('systems', list(BACKEND_ROLES))
    if (not isinstance(systems, list) or not systems or any(not isinstance(system, str) or system not in BACKEND_ROLES for system in systems)
            or len(set(systems)) != len(systems)):
        raise ValueError('systems must be a nonempty unique list of datasystem and/or mooncake')
    return tuple(systems)


def placement_map(value, label):
    if not isinstance(value, dict) or not value:
        raise ValueError(f'{label} requires a nonempty node-to-count object')
    for node, count in value.items():
        if not isinstance(node, str) or len(node) > 253 or not NODE_NAME.fullmatch(node):
            raise ValueError(f'{label}: invalid Kubernetes node name {node!r}')
        positive_integer(count, f'{label}.{node}')
    return tuple(value.items())


def deployment_placements(scenario):
    systems = configured_systems(scenario)
    deployment = scenario.get('deployment')
    if deployment is None:
        nodes = scenario.get('nodes')
        if (not isinstance(nodes, list) or len(nodes) < 2 or any(not isinstance(node, str) for node in nodes)
                or len(set(nodes)) != len(nodes)):
            raise ValueError('legacy nodes must list at least two unique Kubernetes node names')
        count = positive_integer(scenario.get('clients_per_node'), 'clients_per_node')
        clients = placement_map(dict.fromkeys(nodes, count), 'clients')
        control = placement_map({scenario.get('control_node'): 1}, 'control_node')
        placements = tuple((backend, role, control if index == 0 else tuple((n, 1) for n in nodes))
                           for backend in systems for index, role in enumerate(BACKEND_ROLES[backend]))
        return clients, placements
    if not isinstance(deployment, dict) or deployment.get('mode', 'kubernetes') != 'kubernetes':
        raise ValueError('deployment.mode must be kubernetes')
    unknown = set(deployment) - {'mode', 'clients', *BACKEND_ROLES}
    if unknown:
        raise ValueError(f'unknown deployment fields: {sorted(unknown)}')
    clients = placement_map(deployment.get('clients'), 'deployment.clients')
    placements = []
    for backend in systems:
        roles = deployment.get(backend)
        if not isinstance(roles, dict) or set(roles) != set(BACKEND_ROLES[backend]):
            raise ValueError(f'deployment.{backend} requires {BACKEND_ROLES[backend]}')
        for role in BACKEND_ROLES[backend]:
            counts = placement_map(roles[role], f'deployment.{backend}.{role}')
            total = sum(count for _, count in counts)
            if role == 'coordinator' and total not in COORDINATOR_COUNTS:
                raise ValueError('DataSystem coordinator count must be 1, 3 or 5')
            if role == 'master' and total != 1:
                raise ValueError('Mooncake requires exactly one master; this launcher does not implement master HA')
            placements.append((backend, role, counts))
    return clients, tuple(placements)


def build_plan(scenario):
    clients, components = deployment_placements(scenario)
    nodes = tuple(dict.fromkeys(node for _, role, placement in components
                                if role in ('worker', 'store') for node, _ in placement))
    size = scenario_size(scenario)
    rates = scenario.get('target_qps', {})
    qps = rates.get(size) if isinstance(rates, dict) else None
    if not isinstance(qps, list) or len(qps) != 1:
        raise ValueError(f'{size} target QPS must contain one positive integer per trial')
    rate = positive_integer(qps[0], f'target QPS for {size}')
    writers = positive_integer(scenario.get('writer_threads'), 'writer_threads')
    readers = positive_integer(scenario.get('reader_threads', writers), 'reader_threads')
    fanout = positive_integer(scenario.get('notify_count'), 'notify_count')
    repeats = positive_integer(scenario.get('reads_per_notify', 1), 'reads_per_notify')
    include_self = scenario.get('notify_include_self', False)
    if type(include_self) is not bool:
        raise ValueError('notify_include_self must be true or false')
    counts = {count for _, count in clients}
    uniform_count = next(iter(counts)) if len(counts) == 1 else 0
    return Plan(nodes, uniform_count, rate, writers, fanout, size, readers, clients, components, include_self, repeats)


def expand_scenarios(scenario):
    sizes = scenario.get('sizes', [DEFAULT_DATA_SIZE])
    rates = scenario.get('target_qps')
    if not isinstance(sizes, list) or not sizes or not isinstance(rates, dict):
        raise ValueError('sizes and target_qps require a nonempty size list and rate object')
    if any(not isinstance(size, str) for size in sizes) or len(set(sizes)) != len(sizes) or set(rates) != set(sizes):
        raise ValueError('sizes must be unique and match target_qps keys exactly')
    trials = []
    for size in sizes:
        data_size_bytes(size)
        values = rates[size]
        if not isinstance(values, list) or not values:
            raise ValueError(f'target_qps.{size} requires a nonempty positive integer list')
        if any(type(rate) is not int or rate <= 0 for rate in values) or len(set(values)) != len(values):
            raise ValueError(f'target_qps.{size} requires unique positive integers')
        for rate in values:
            trial = copy.deepcopy(scenario)
            trial.update(sizes=[size], target_qps={size: [rate]})
            build_plan(trial)
            trials.append(trial)
    return trials
