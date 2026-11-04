"""Preserve dscli template text while replacing existing value tokens."""

import json


RUNTIME_CONFIG_KEYS = frozenset({
    'worker_address', 'coordinator_address', 'coordinator_raft_initial_peers',
    'host_id_env_name', 'cluster_name', 'log_dir', 'coordinator_raft_data_dir',
    'unix_domain_socket_dir', 'health_check_path', 'ready_check_path',
    'monitor_config_file', 'rocksdb_store_dir', 'spill_directory',
})


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'duplicate template key: {key}')
        result[key] = value
    return result


def _fields(source, start):
    decoder = json.JSONDecoder()
    position = start + 1
    while True:
        while source[position].isspace():
            position += 1
        if source[position] == '}':
            return
        key, position = decoder.raw_decode(source, position)
        while source[position].isspace():
            position += 1
        position += 1
        while source[position].isspace():
            position += 1
        value_start = position
        _, position = decoder.raw_decode(source, position)
        yield key, value_start, position
        while source[position].isspace():
            position += 1
        if source[position] == ',':
            position += 1


def render_ds_config(source, config, allowed_keys=None):
    original = json.loads(source, object_pairs_hook=_unique_object)
    if not isinstance(original, dict) or not isinstance(config, dict) or list(config) != list(original):
        raise ValueError('strict template parameters must not be added, removed or reordered')
    changed = {}
    for key, entry in original.items():
        updated = config[key]
        if (not isinstance(entry, dict) or 'value' not in entry
                or not isinstance(updated, dict) or list(updated) != list(entry)
                or any(updated[field] != value for field, value in entry.items() if field != 'value')):
            raise ValueError(f'strict template structure/description changed: {key}')
        if type(updated['value']) is not type(entry['value']) or updated['value'] != entry['value']:
            if allowed_keys is not None and key not in allowed_keys:
                raise ValueError(f'strict template value cannot be overridden: {key}')
            changed[key] = updated['value']
    replacements = []
    for key, start, _ in _fields(source, len(source) - len(source.lstrip())):
        if key in changed:
            for field, begin, end in _fields(source, start):
                if field == 'value':
                    replacements.append((begin, end, json.dumps(changed[key], ensure_ascii=False)))
    for begin, end, value in reversed(replacements):
        source = source[:begin] + value + source[end:]
    return source
