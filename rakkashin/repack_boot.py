# Replace the kernel in an Android boot image (header v3/v4) and refresh the AVB hash footer.
import hashlib, struct, sys

src, kernel_path, out = sys.argv[1:4]
img = open(src, "rb").read()
kernel = open(kernel_path, "rb").read()
PAGE = 4096
pad = lambda n: (n + PAGE - 1) // PAGE * PAGE

assert img[:8] == b"ANDROID!"
kernel_size, ramdisk_size, os_version, header_size = struct.unpack_from("<4I", img, 8)
header_version = struct.unpack_from("<I", img, 40)[0]
assert header_version in (3, 4), header_version
sig_size = struct.unpack_from("<I", img, 1580)[0] if header_version == 4 else 0
print(f"hdr v{header_version} kernel={kernel_size} ramdisk={ramdisk_size} sig={sig_size}")
assert kernel[0x38:0x3c] == b"ARMd", "new kernel is not a raw arm64 Image"
old_k = img[PAGE:PAGE + kernel_size]
assert old_k[0x38:0x3c] == b"ARMd", "stock kernel is not a raw arm64 Image"

# AVB footer
footer = img[-64:]
assert footer[:4] == b"AVBf"
fmaj, fmin, orig_size, vb_off, vb_size = struct.unpack(">2I3Q", footer[4:36])
vbmeta = bytearray(img[vb_off:vb_off + vb_size])
off = PAGE + pad(kernel_size)
rest = img[off:off + pad(ramdisk_size) + pad(sig_size)]  # ramdisk + boot signature, unchanged

hdr = bytearray(img[:PAGE])
struct.pack_into("<I", hdr, 8, len(kernel))
body = bytes(hdr) + kernel + b"\0" * (pad(len(kernel)) - len(kernel)) + rest
new_orig = len(body)

# vbmeta: header 256 bytes, auth block, aux block; find hash descriptor (tag 2)
auth_size, aux_size = struct.unpack_from(">2Q", vbmeta, 12)
desc_off, desc_size = struct.unpack_from(">2Q", vbmeta, 96)
base = 256 + auth_size
p, end, done = base + desc_off, base + desc_off + desc_size, False
while p < end:
    tag, nbf = struct.unpack_from(">2Q", vbmeta, p)
    if tag == 2:
        image_size, = struct.unpack_from(">Q", vbmeta, p + 16)
        algo = bytes(vbmeta[p + 24:p + 56]).rstrip(b"\0").decode()
        name_len, salt_len, dig_len = struct.unpack_from(">3I", vbmeta, p + 56)
        q = p + 16 + 116  # after fixed fields (incl. flags + reserved)
        name = bytes(vbmeta[q:q + name_len]).decode(); q += name_len
        salt = bytes(vbmeta[q:q + salt_len]); q += salt_len
        assert algo == "sha256" and image_size == orig_size, (algo, image_size, orig_size)
        assert hashlib.sha256(salt + img[:orig_size]).digest() == bytes(vbmeta[q:q + dig_len]), "stock digest mismatch"
        struct.pack_into(">Q", vbmeta, p + 16, new_orig)
        vbmeta[q:q + dig_len] = hashlib.sha256(salt + body).digest()
        print(f"hash descriptor '{name}': {image_size} -> {new_orig}")
        done = True
    p += 16 + nbf
assert done

part_size = len(img)
new_vb_off = pad(new_orig)
outb = bytearray(part_size)
outb[:new_orig] = body
outb[new_vb_off:new_vb_off + vb_size] = vbmeta
outb[-64:] = footer[:4] + struct.pack(">2I3Q", fmaj, fmin, new_orig, new_vb_off, vb_size) + footer[36:]
assert new_vb_off + vb_size <= part_size - 64
open(out, "wb").write(outb)
print("wrote", out, len(outb))
