"""Decal records kept in Fusion attributes under the group cubevox-decal.

The inlay body carries its decal's record as `settings`. The design carries the list of decal
names as `index` and a copy of each record as `decal:<name>`, because a suppressed decal's
inlay body is gone from the model and a rebake still has to read it. The stored cut body
carries `stored`, naming its decal and the body it was cut from.

A record is a dict with name, settings (the dialog's dict), source, face (kind, click, normal,
axis, centre, radius, component, body, occurrence), inlay_body, stored_body and made.
"""

import json

from decal_core.errors import DecalError

from . import ATTR_GROUP

INDEX = 'index'
SETTINGS = 'settings'
STORED = 'stored'
RECORD = 'decal:'


def _read(attributes, name: str):
    attr = attributes.itemByName(ATTR_GROUP, name)
    return None if attr is None else attr.value


def _write(attributes, name: str, value: str, owner: str) -> None:
    if attributes.add(ATTR_GROUP, name, value) is None:
        raise DecalError(f'Fusion did not store the attribute {ATTR_GROUP}/{name} on {owner}.')


def _parse(raw: str, what: str):
    try:
        return json.loads(raw)
    except json.JSONDecodeError as e:
        raise DecalError(f'The stored {what} is not valid JSON: {e}.') from e


def encode_settings(settings: dict) -> dict:
    """Settings with hole_keepouts keyed by text, as JSON needs."""
    out = dict(settings)
    out['hole_keepouts'] = {f'{float(k):.1f}': float(v) for k, v in settings['hole_keepouts'].items()}
    return out


def decode_settings(settings: dict) -> dict:
    """Settings with hole_keepouts keyed by the diameter rounded to 0.1 mm."""
    out = dict(settings)
    out['hole_keepouts'] = {round(float(k), 1): float(v) for k, v in settings.get('hole_keepouts', {}).items()}
    return out


def names(design) -> list:
    """Every decal name in the design index, oldest first."""
    raw = _read(design.attributes, INDEX)
    return [] if raw is None else list(_parse(raw, 'decal index'))


def load(design, name: str):
    """The record of one decal, or None when the design has none under that name."""
    raw = _read(design.attributes, RECORD + name)
    if raw is None:
        return None
    rec = _parse(raw, f'record of decal {name}')
    rec['settings'] = decode_settings(rec['settings'])
    return rec


def save(design, inlay_body, record: dict) -> None:
    """Store the record on the inlay body and in the design, and list the name."""
    rec = dict(record)
    rec['settings'] = encode_settings(record['settings'])
    text = json.dumps(rec, sort_keys=True)
    _write(inlay_body.attributes, SETTINGS, text, inlay_body.name)
    _write(design.attributes, RECORD + rec['name'], text, 'the design')
    listed = names(design)
    if rec['name'] not in listed:
        listed.append(rec['name'])
    _write(design.attributes, INDEX, json.dumps(listed), 'the design')


def forget(design, name: str) -> None:
    """Drop a decal's design record and its index entry."""
    attr = design.attributes.itemByName(ATTR_GROUP, RECORD + name)
    if attr is not None and not attr.deleteMe():
        raise DecalError(f'Fusion did not delete the stored record of decal {name}.')
    listed = [n for n in names(design) if n != name]
    _write(design.attributes, INDEX, json.dumps(listed), 'the design')


def mark_stored(body, decal: str, base: str) -> None:
    """Mark a stored cut body with its decal and the body it was cut from."""
    _write(body.attributes, STORED, json.dumps({'decal': decal, 'body': base}), body.name)


def _records_in(design, component_name: str):
    for n in names(design):
        rec = load(design, n)
        if rec is not None and rec['face']['component'] == component_name:
            yield rec


def base_of(design, body) -> str:
    """The name of the body a decal is cut from: the body itself, or for a stored cut body
    the body its decal was cut from. The design records cover a body whose attribute is gone."""
    raw = _read(body.attributes, STORED)
    if raw is not None:
        return _parse(raw, f'marker on {body.name}')['body']
    for rec in _records_in(design, body.parentComponent.name):
        if rec.get('stored_body') == body.name:
            return rec['face']['body']
    return body.name


def inlay_of(design, body):
    """The decal name when body is a decal's inlay body, else None."""
    raw = _read(body.attributes, SETTINGS)
    if raw is not None:
        return _parse(raw, f'settings on {body.name}')['name']
    for rec in _records_in(design, body.parentComponent.name):
        if rec.get('inlay_body') == body.name:
            return rec['name']
    return None
