import re, sys

BS = chr(92)
h = open('yt.html', encoding='utf-8', errors='replace').read()

k = '"shortDescription":"'
i = h.find(k)
if i < 0:
    print('no description')
    sys.exit(0)
i += len(k)
j = i
while j < len(h):
    c = h[j]
    if c == BS:
        j += 2
        continue
    if c == '"':
        break
    j += 1

raw = h[i:j]
out = []
k2 = 0
while k2 < len(raw):
    c = raw[k2]
    if c == BS and k2 + 1 < len(raw):
        n = raw[k2 + 1]
        mp = {'n': '\n', 't': '\t', '"': '"', BS: BS, '/': '/'}
        if n == 'u':
            try:
                out.append(chr(int(raw[k2 + 2:k2 + 6], 16)))
                k2 += 6
                continue
            except ValueError:
                pass
        out.append(mp.get(n, n))
        k2 += 2
        continue
    out.append(c)
    k2 += 1

desc = ''.join(out)
print('=== DESCRIPTION ===')
print(desc[:2500])
print()
print('=== URLs ===')
for u in re.findall(r'https?://[A-Za-z0-9._~:/?#@!$&()*+,;=%-]+', desc):
    print(u)
