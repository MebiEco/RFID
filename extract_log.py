
import json

log_path = r'C:\Users\ntk14\.gemini\antigravity\brain\6f1f61b4-595c-4f4e-a255-9a0effac74c2\.system_generated\logs\overview.txt'

with open(log_path, 'r', encoding='utf-8') as f:
    for line in f:
        if 'step_index":155' in line:
            print(line)
            break
