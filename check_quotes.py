import re

with open('original_targets_utf8.xml', 'r', encoding='utf-8') as f:
    content = f.read()

match = re.search(r'<Exec Command="([^"]+)"', content)
if match:
    print(repr(match.group(1)[:150]))