import os
import glob

def replace_in_file(filepath):
    with open(filepath, 'r') as f:
        content = f.read()
    
    replacements = {
        "Lithium Engine": "Sodium Engine",
        "LITHIUM ENGINE": "SODIUM ENGINE",
        "Lithium_Engine": "Sodium_Engine",
        "Lithium": "Sodium",
        "LITHIUM": "SODIUM",
        "HARTRE Mode": "Sodium Real-Time",
        "HARTRE": "Sodium",
        
        "Perill": "Tesla",
        "PERILL": "TESLA",
        "perill_": "tesla_",
        "enable_perill": "enable_tesla",
        "is_perill_mode": "is_tesla_mode",
        "set_perill_mode": "set_tesla_mode",
        "perill_mode": "tesla_mode"
    }
    
    new_content = content
    for old, new in replacements.items():
        new_content = new_content.replace(old, new)
        
    if new_content != content:
        with open(filepath, 'w') as f:
            f.write(new_content)
        print(f"Updated {filepath}")

for root, _, files in os.walk("src"):
    for file in files:
        if file.endswith((".cpp", ".hpp", ".h", ".c")):
            replace_in_file(os.path.join(root, file))

for root, _, files in os.walk("include"):
    for file in files:
        if file.endswith((".cpp", ".hpp", ".h", ".c")):
            replace_in_file(os.path.join(root, file))
