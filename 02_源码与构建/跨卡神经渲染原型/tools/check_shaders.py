import io, re, subprocess, glob, os, sys

cpp = r'E:\AI\跨卡神经渲染原型\ingame\libxess_proxy_v7.cpp'
s = io.open(cpp, encoding='utf-8').read()
m = re.search(r'R"\((.*?)\)";', s, re.S)
if not m:
    print('HLSL not found'); sys.exit(1)
hlsl = m.group(1)
out = r'E:\AI\跨卡神经渲染原型\build\v7_shaders.hlsl'
io.open(out, 'w', encoding='utf-8', newline='\n').write(hlsl)
print('HLSL extracted ->', out, len(hlsl), 'bytes')

fxc = None
for pat in [r'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\fxc.exe',
            r'C:\Program Files\Windows Kits\10\bin\*\x64\fxc.exe']:
    hits = glob.glob(pat)
    if hits:
        fxc = sorted(hits)[-1]
        break
print('fxc =', fxc)
if not fxc:
    sys.exit(0)

for entry in ['ResampleCS', 'CaptureCS', 'EditCS', 'MarkCS']:
    r = subprocess.run([fxc, '/T', 'cs_5_0', '/E', entry, '/O3', '/nologo', out,
                        '/Fo', r'E:\AI\跨卡神经渲染原型\build\v7_%s.cso' % entry],
                       capture_output=True, text=True)
    tag = 'OK ' if r.returncode == 0 else 'FAIL'
    print('[%s] %s %s' % (tag, entry, (r.stdout + r.stderr).strip()[:400]))
