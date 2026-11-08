"""A small LZ77 packer for the self-extracting test programs (the depacker is depack.asm).

Format: a token byte, then
  $00-$7F  literal run: token + 1 bytes follow
  $80-$FE  match: length (token & $7F) + 3, followed by the distance back (2 bytes, low byte first)
  $FF      end
"""


def pack(data):
    out = bytearray()
    literals = bytearray()

    def flush():
        while literals:
            chunk = literals[:128]
            out.append(len(chunk) - 1)
            out.extend(chunk)
            del literals[:128]

    i = 0
    while i < len(data):
        best_len, best_dist = 0, 0
        for start in range(max(0, i - 4096), i):
            n = 0
            while n < 127 + 3 - 1 and i + n < len(data) and data[start + n] == data[i + n]:
                n += 1
            if n > best_len:
                best_len, best_dist = n, i - start
        if best_len >= 3:
            flush()
            out.append(0x80 + best_len - 3)
            out.extend((best_dist & 0xFF, best_dist >> 8))
            i += best_len
        else:
            literals.append(data[i])
            i += 1
    flush()
    out.append(0xFF)
    return bytes(out)


def unpack(packed):
    """The reference depacker (what depack.asm does)."""
    out = bytearray()
    i = 0
    while True:
        t = packed[i]
        i += 1
        if t == 0xFF:
            return bytes(out)
        if t < 0x80:
            out.extend(packed[i:i + t + 1])
            i += t + 1
        else:
            n, d = (t & 0x7F) + 3, packed[i] | packed[i + 1] << 8
            i += 2
            for _ in range(n):
                out.append(out[-d])
