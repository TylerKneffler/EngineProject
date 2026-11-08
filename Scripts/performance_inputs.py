"""Scene/prefab content identity for local performance captures (not all assets)."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET


def resolve_input(reference, root):
    """Mirror Mesh::ResolveFilePath's checkout-relative and bundled fallbacks."""
    reference = str(reference).replace('\\', '/')
    path = Path(reference)
    requested = path if path.is_absolute() else root / path
    if requested.exists():
        return requested.resolve()
    marker = 'Engine/Core/Assets/'
    if marker in reference:
        relocated = root / marker / reference.split(marker, 1)[1]
        if relocated.exists():
            return relocated.resolve()
    if reference.startswith('Assets/'):
        bundled = root / marker / reference[len('Assets/'):]
        if bundled.exists():
            return bundled.resolve()
    return requested.resolve()


def document_references(path):
    """Discover literal scene/prefab references; stream large XML placement files."""
    def reference(value):
        return isinstance(value, str) and value.strip().lower().endswith(('.scene', '.prefab'))

    with path.open('rb') as stream:
        prefix = stream.read(4096)
        if prefix.startswith(b'\xef\xbb\xbf'):
            prefix = prefix[3:]
        prefix = prefix.lstrip()
        while not prefix:
            chunk = stream.read(4096)
            if not chunk:
                break
            prefix = chunk.lstrip()
    # XML BOMs/UTF-16 are handled by ElementTree, just as by the engine parser.
    if prefix.startswith((b'<', b'\xff\xfe', b'\xfe\xff')):
        stack = []
        for event, node in ET.iterparse(path, events=('start', 'end')):
            if event == 'start':
                stack.append(node)
                for value in node.attrib.values():
                    if reference(value):
                        yield value.strip()
            else:
                if reference(node.text):
                    yield node.text.strip()
                stack.pop()
                node.clear()
                if stack:
                    stack[-1].remove(node)
    else:
        values = [json.loads(path.read_text(encoding='utf-8-sig'))]
        while values:
            value = values.pop()
            if isinstance(value, dict):
                values.extend(value.values())
            elif isinstance(value, list):
                values.extend(value)
            elif reference(value):
                yield value.strip()


def snapshot_inputs(scene, additional=(), root=None):
    root = Path(root or Path(__file__).resolve().parents[1]).resolve()
    pending = [str(scene), *(str(p) for p in additional)]
    requests, files, errors = {}, {}, []
    while pending:
        request = pending.pop()
        if request in requests:
            continue
        path = resolve_input(request, root)
        key = str(path)
        requests[request] = key
        if key in files:
            continue
        try:
            before = path.stat()
            with path.open('rb') as stream:
                digest = hashlib.sha256()
                for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(chunk)
            files[key] = dict(size_bytes=before.st_size, mtime_ns=before.st_mtime_ns,
                              sha256=digest.hexdigest())
            if path.suffix.lower() in ('.scene', '.prefab'):
                pending.extend(document_references(path))
            after = path.stat()
            if (before.st_size, before.st_mtime_ns, before.st_ino) != (
                    after.st_size, after.st_mtime_ns, after.st_ino):
                errors.append(dict(path=key, error='Input changed while fingerprinting'))
        except (OSError, ValueError, ET.ParseError) as error:
            errors.append(dict(path=key, error=str(error)))
    return dict(recorded_at_utc=datetime.now(timezone.utc).isoformat(),
                requests=dict(sorted(requests.items())), files=dict(sorted(files.items())),
                errors=errors)


def compare_inputs(before, after):
    changed = sorted(key for key in before['files'].keys() | after['files'].keys()
                     if before['files'].get(key) != after['files'].get(key))
    relocated = sorted(key for key in before['requests'].keys() | after['requests'].keys()
                       if before['requests'].get(key) != after['requests'].get(key))
    complete = not before['errors'] and not after['errors']
    return dict(complete=complete, unchanged=complete and not changed and not relocated,
                changed_files=changed, changed_resolutions=relocated)
