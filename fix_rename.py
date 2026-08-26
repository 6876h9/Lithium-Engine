import os

def fix_in_file(filepath):
    with open(filepath, 'r') as f:
        content = f.read()
    
    replacements = {
        "Sodium Engine Launcher": "Lithium Engine Launcher",
        "SODIUM ENGINE LAUNCHER": "LITHIUM ENGINE LAUNCHER",
        "Sodium C++ Game Engine": "Lithium C++ Game Engine",
        "[Sodium Engine]": "[Lithium Engine]",
        "Sodium Editor Workspace": "Lithium Editor Workspace",
        "Sodium_Engine": "Lithium_Engine",
        "add_executable(Sodium_Engine": "add_executable(Lithium_Engine",
        "target_include_directories(Sodium_Engine": "target_include_directories(Lithium_Engine",
        "target_link_directories(Sodium_Engine": "target_link_directories(Lithium_Engine",
        "target_link_libraries(Sodium_Engine": "target_link_libraries(Lithium_Engine",
        "CMakeFiles/Sodium_Engine.dir": "CMakeFiles/Lithium_Engine.dir"
    }
    
    new_content = content
    for old, new in replacements.items():
        new_content = new_content.replace(old, new)
        
    if new_content != content:
        with open(filepath, 'w') as f:
            f.write(new_content)
        print(f"Fixed {filepath}")

for root, _, files in os.walk("src"):
    for file in files:
        if file.endswith((".cpp", ".hpp", ".h", ".c")):
            fix_in_file(os.path.join(root, file))

for root, _, files in os.walk("include"):
    for file in files:
        if file.endswith((".cpp", ".hpp", ".h", ".c")):
            fix_in_file(os.path.join(root, file))
            
fix_in_file("CMakeLists.txt")
