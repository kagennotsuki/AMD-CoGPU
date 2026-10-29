import struct, sys, io

p = r'E:\SteamLibrary\steamapps\common\RESIDENT EVIL 2  BIOHAZARD RE2\reframework_crash.dmp'
d = open(p, 'rb').read()
print('dump size', len(d))

sig = d[:4]
if sig != b'MDMP':
    print('not a minidump:', sig); sys.exit(1)

(ver, nstreams, dir_rva) = struct.unpack_from('<III', d, 4)
print('version %x streams %d dir %x' % (ver, nstreams, dir_rva))

streams = {}
for i in range(nstreams):
    typ, sz, rva = struct.unpack_from('<III', d, dir_rva + i * 12)
    streams[typ] = (sz, rva)

# Stream types: 3=ThreadList 4=ModuleList 6=Exception 7=SystemInfo 9=MemoryList
mods = []
if 4 in streams:
    sz, rva = streams[4]
    n = struct.unpack_from('<I', d, rva)[0]
    for i in range(n):
        off = rva + 4 + i * 108
        base = struct.unpack_from('<Q', d, off)[0]
        size = struct.unpack_from('<I', d, off + 8)[0]
        name_rva = struct.unpack_from('<I', d, off + 20)[0]
        # MINIDUMP_STRING: u32 byte length then UTF-16
        if name_rva and name_rva < len(d):
            blen = struct.unpack_from('<I', d, name_rva)[0]
            raw = d[name_rva + 4: name_rva + 4 + blen]
            try:
                name = raw.decode('utf-16-le')
            except Exception:
                name = repr(raw[:40])
        else:
            name = '?'
        mods.append((base, size, name))

print('\n=== 模块（仅 7fff10/7fff11 段，便于对照 RIP）===')
for base, size, name in sorted(mods):
    if 0x7fff10000000 <= base <= 0x7fff13000000:
        print('  %016x + %08x  %s' % (base, size, name))

exc_addr = None
if 6 in streams:
    sz, rva = streams[6]
    tid = struct.unpack_from('<I', d, rva)[0]
    code = struct.unpack_from('<I', d, rva + 8)[0]
    flags = struct.unpack_from('<I', d, rva + 12)[0]
    addr = struct.unpack_from('<Q', d, rva + 24)[0]
    nparam = struct.unpack_from('<I', d, rva + 32)[0]
    params = struct.unpack_from('<15Q', d, rva + 40)
    print('\n=== 异常 ===')
    print('  thread id   = %d' % tid)
    print('  code        = 0x%08x' % code)
    print('  address     = 0x%016x' % addr)
    print('  参数        =', ' '.join('%016x' % x for x in params[:nparam]))
    exc_addr = addr

# thread list: find the faulting thread's start address
if 3 in streams:
    sz, rva = streams[3]
    n = struct.unpack_from('<I', d, rva)[0]
    for i in range(n):
        off = rva + 4 + i * 48
        tid = struct.unpack_from('<I', d, off)[0]
        stack_start = struct.unpack_from('<Q', d, off + 24)[0]
        stack_size = struct.unpack_from('<I', d, off + 32)[0]
        teb = struct.unpack_from('<Q', d, off + 40)[0]
        print('  thread %-6d stack %016x (%d)' % (tid, stack_start, stack_size))

if exc_addr is not None:
    print('\n=== 故障地址归属 ===')
    hit = False
    for base, size, name in mods:
        if base <= exc_addr < base + size:
            print('  %s  + 0x%x' % (name, exc_addr - base))
            hit = True
    if not hit:
        print('  没有模块覆盖该地址（可能是 JIT/堆内存或已卸载模块）')
