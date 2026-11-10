"""Read Delphi binary form resources for an offline settings audit.

No Windows automation or device I/O. Binary/image properties are represented by
size and digest; proprietary assets are not copied to the resulting inventory.
"""
import struct
import hashlib


class DFM:
    def __init__(self, data):
        self.data, self.at = data, 0

    def take(self, n):
        if n < 0 or self.at + n > len(self.data):
            raise ValueError(f'truncated DFM at {self.at:x}')
        out = self.data[self.at:self.at+n]
        self.at += n
        return out

    def num(self, fmt):
        return struct.unpack('<' + fmt, self.take(struct.calcsize('<' + fmt)))[0]

    def string(self):
        return self.take(self.num('B')).decode('cp1252', errors='replace')

    def value(self, kind=None):
        t = self.num('B') if kind is None else kind
        if t in (0, 13): return None
        if t == 1:
            result = []
            while self.data[self.at] != 0: result.append(self.value())
            self.at += 1
            return result
        if t in (2, 3, 4, 15, 16, 17, 19, 21):
            return self.num({2:'b', 3:'h', 4:'i', 15:'f', 16:'q', 17:'d', 19:'q', 21:'d'}[t])
        if t == 5:
            raw = self.take(10)
            mantissa, exp = struct.unpack('<QH', raw)
            sign = -1 if exp & 0x8000 else 1
            exp &= 0x7fff
            return sign * mantissa * (2.0 ** (exp - 16383 - 63)) if exp else 0.0
        if t in (6, 7): return self.string()
        if t == 8: return False
        if t == 9: return True
        if t == 10:
            data = self.take(self.num('I'))
            return {'binary_bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        if t == 11:
            result = []
            while self.data[self.at]: result.append(self.string())
            self.at += 1
            return result
        if t in (12, 20): return self.take(self.num('I')).decode('utf-8' if t == 20 else 'cp1252', errors='replace')
        if t == 18: return self.take(self.num('I') * 2).decode('utf-16le', errors='replace')
        if t == 14:
            result = []
            while self.data[self.at] != 0:
                tag = self.num('B')
                if tag != 1: raise ValueError(f'collection marker {tag} at {self.at:x}')
                result.append(self.properties())
            self.at += 1
            return result
        raise ValueError(f'unknown value type {t} at {self.at:x}')

    def properties(self):
        out = {}
        while self.data[self.at]:
            name = self.string()
            out[name] = self.value()
        self.at += 1
        return out

    def component(self):
        prefix = None
        if self.data[self.at] & 0xf0 == 0xf0:
            flags = self.num('B') & 0x0f
            prefix = {'flags': flags}
            if flags & 2: prefix['child_position'] = self.value()
        cls, name = self.string(), self.string()
        result = {'class': cls, 'name': name, 'properties': self.properties(), 'children': []}
        if prefix: result['prefix'] = prefix
        while self.data[self.at]: result['children'].append(self.component())
        self.at += 1
        return result

    def parse(self):
        if self.take(4) != b'TPF0': raise ValueError('not a binary Delphi form')
        result = self.component()
        if self.at != len(self.data): raise ValueError(f'{len(self.data)-self.at} trailing bytes')
        return result


def walk(node, parent=''):
    path = parent + '/' + node['name']
    yield path, node
    for child in node['children']: yield from walk(child, path)
