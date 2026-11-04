"""Validation for environment maps passed to remote service processes."""
import re


def validate_environment(env, name, reserved=()):
    if not isinstance(env, dict) or any(
            not isinstance(key, str) or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', key) or
            key in reserved or not isinstance(value, str) or '\x00' in value
            for key, value in env.items()):
        raise ValueError(f'{name} must contain valid non-reserved environment names and string values')
    return dict(env)
