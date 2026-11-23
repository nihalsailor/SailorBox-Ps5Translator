import re

with open('C:/Developments/@SailorBox/src/tools/agc_aliases.txt', 'r') as f:
    lines = f.readlines()

names = []
for l in lines:
    m = re.search(r'AddNidAlias\("([^"]+)",\s*"([^"]+)"\)', l)
    if m:
        names.append(m.group(2))

unique = sorted(list(set(names)))
print(f"Total aliases: {len(names)}")
print(f"Unique function names: {len(unique)}")

with open('C:/Developments/@SailorBox/src/tools/unique_agc_exports.txt', 'w') as f:
    for u in unique:
        f.write(u + '\n')
