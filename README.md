# Rip & Tear

<p align="center">
    <img src="https://img.shields.io/badge/Spelunky 2-1.25.2-orange">
    <a href="https://isocpp.org/">
        <img src="https://img.shields.io/badge/language-C%2B%2B20-blue.svg">
    </a>
    <a href="https://github.com/Malacath-92/RipAndTear/actions">
        <img src="https://github.com/Malacath-92/RipAndTear/workflows/Github%20Actions%20CI/badge.svg">
    </a>
    <a href="https://github.com/Malacath-92/RipAndTear/actions">
        <img src="https://github.com/Malacath-92/RipAndTear/workflows/Formatting/badge.svg">
    </a>
    <a href="https://opensource.org/licenses/MIT" >
        <img src="https://img.shields.io/apm/l/vim-mode.svg">
    </a>
</p>

This program rips all assets from the Spelunky 2 executable and shrinks the resulting executable to fit. It then patches the executable to load all files from disk.

## Build
Only Windows build is supported:
```sh
git clone git@github.com:Malacath-92/RipAndTear.git rip_and_tear
cd rip_and_tear
git submodule update --init --recursive
mkdir build
cd build
cmake ..
cmake --build . --config Release
cmake --install .
```
Build artifacts are found in the `publish` folder.

### Requirements
- MSVC 2019 (for C++20)
- python
    - cmake
    - conan
- clang-format
