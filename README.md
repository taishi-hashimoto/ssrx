# ssrx

C++ Secondary Surveillance Radar (SSR) Mode-S receiver software using HackRF/USRP.

## Key Features

- Real-time decoding of Mode-S/ADS-B messages using **HackRF** and **USRP**.
- High sampling rates (e.g., ~12 MSPS on Raspberry Pi 4/5) for improved decoding performance, compared to ~2.4 MSPS for typical RTL-SDR based devices.
- **Raw waveform** retrieval for research purposes.
- ZeroMQ-based collector protocol for downstream processing.
- PPS synchronization support for accurate timestamping (USRP only).

## Dependencies

Common C++ build dependencies:

- CMake 3.20 or later.
- C++20 compiler.
- CLI11.
- FFTW3 single precision.
- SQLite3.
- yaml-cpp.
- ZeroMQ and cppzmq.
- ncurses or PDCurses for `ssrx-rbstat` and sampler `--amphist` mode for amplitude histogram.

Device-specific dependencies:

- HackRF development library ([libhackrf](https://github.com/greatscottgadgets/hackrf)) for `ssrx-hackrf`.
- USRP Hardware Driver ([UHD](https://github.com/ettusresearch/uhd)) for `ssrx-usrp` (Optional).

Ubuntu example:

```bash
sudo apt-get update
sudo apt-get install cmake pkg-config libhackrf-dev libncursesw5-dev \
  libsqlite3-dev libyaml-cpp-dev libzmq3-dev cppzmq-dev libfftw3-dev \
  libcli11-dev libusb-1.0-0-dev
# Optional UHD for USRP support:
# sudo apt-get install libuhd-dev
```

macOS Homebrew example:

```bash
brew install cmake pkg-config hackrf ncurses sqlite yaml-cpp zeromq cppzmq cli11 libusb fftw
# Optional UHD for USRP support:
# brew install uhd
```

For Windows, the install helper script below sets up most of the dependencies using [vcpkg.json](./vcpkg.json).  
For USRP support, you need to install UHD manually using official installers from Ettus Research.  
See the official instructions [here](https://uhd.readthedocs.io/en/latest/page_install_binary.html#install_win).

## Build

### Linux/macOS

Just type `make && make install`, or use CMake explicitly for more control.

Default builds include HackRF and rbstat. USRP is opt-in.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

If HackRF headers or libraries are not available:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSSRX_BUILD_HACKRF=OFF
cmake --build build -j
```

To build the optional USRP target:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSSRX_BUILD_USRP=ON
cmake --build build -j
```

Install binaries:

```bash
cmake --install build --prefix ~/.ssrx
```

### Windows

*NOTE: Windows support is currently experimental and may require manual configuration.*

Windows MSVC/vcpkg helpers are provided in [CMakePresets.json](./CMakePresets.json) and
[scripts/build-windows.ps1](./scripts/build-windows.ps1).

Run the PowerShell script to set up the vcpkg environment and build the project:

```powershell
.\scripts\build-windows.ps1
```

Or, USRP only build:

```powershell
.\scripts\build-windows.ps1 -SkipHackRFHost -EnableUSRP -UhdRoot "C:\Program Files\UHD"
```

*NOTE: these will generate release builds of `ssrx-hackrf`, `ssrx-usrp` and `ssrx-rbstat` under `build\Release`.*

## Configuration

The sampler config path is positional and defaults to `~/.ssrx/conf/ssrx.yaml`.
Use [conf/ssrx.yaml](./conf/ssrx.yaml) as the starting template.

```bash
mkdir -p ~/.ssrx/conf
cp conf/ssrx.yaml ~/.ssrx/conf/ssrx.yaml
```

*NOTE: This example configuration is intended to be a reasonable default for Raspberry Pi 4/5 equipped with HackRF One/Pro or USRP B200 and a small dipole antenna without pre-amplification.*

## CLI

- `ssrx-hackrf`: Mode-S receiver using **HackRF One/Pro**.
- `ssrx-usrp`: Mode-S receiver using **USRP**.
- `ssrx-rbstat`: Terminal monitor for sampler ring-buffer state.

Example systemd user units for the C++ samplers are in [services/](./services/).

### Example Usage

```bash
$ build/ssrx-hackrf --help

SSR receiver program using HackRF device.
Usage: build/ssrx-hackrf [OPTIONS] [config]

Positionals:
  config TEXT                 Path to YAML configuration file.

Options:
  -h,--help                   Shows help message.
  -v,--version                Shows version number.
  --test                      Test configuration and HackRF, then exit.
  --test-integrity            Test ringbuffer integrity.
  -a,--amphist                Start amplitude histogram to check the signal quality.
  -n,--no-pps                 Start sampling without latching time with the next PPS (no effect for HackRF).
```

`ssrx-usrp` has the same CLI options. When your environment omits PPS source, use `--no-pps` to start sampling without PPS synchronization.

Run HackRF sampler:

```bash
~/.ssrx/bin/ssrx-hackrf ~/.ssrx/conf/ssrx.yaml
```

Monitor the ring buffer from another terminal:

```bash
ssrx-rbstat ~/.ssrx/conf/ssrx.yaml
```

Amplitude histogram plot for signal quality assessment:

```bash
ssrx-hackrf -a ~/.ssrx/conf/ssrx.yaml
```

This will output something like below:

```
AmpHist  bins=105  full_scale=1  max_count=500  [q:quit r:reset]

     -- -----
     | - ||  -
     || |||   -
     || |||    --
   - ||||||||    -
    -|||||||||     --
     ||||||||||   -
  -| ||||||||||||    -----
   |||||||||||||||        ---
  ||||||||||||||||| |        --
  |||||||||||||||||||||||      ----
 -||||||||||||||||||||||||||||     ----- --- -
 ||||||||||||||||||||||||||||||||||| |  -   - -------------------------------------------   --
Tip: right end (=full_scale) indicates saturation/clipping
```

The right end of the histogram indicates the full scale of the ADC (float32).
Max-hold is indicated by `-`. When clipping occurrs, `x` will be shown at the right end, indicating the input signal is too strong or preamplifier setting is too high.

## Collector Protocol

`ssrx-hackrf` and `ssrx-usrp` publish decoded messages through the
`collector.mq` endpoint in [conf/ssrx.yaml](./conf/ssrx.yaml). The collector sends each decoded
message as a ZeroMQ multipart message:

1. Binary `ssrx::Header`.
2. Raw Mode-S message bytes (`msg_len` bytes).
3. Optional waveform payload (`nsymbols` * `sizeof(std::complex<float>)` bytes).

The current C++ header layout is:

```cpp
struct Header {
    int64_t seconds;
    int64_t femtoseconds;
    double rssi;
    uint32_t msg_len;
    uint32_t ncoh;
    uint32_t nsymbols;
    uint32_t vtype;
};
```

`vtype` is currently fixed to `VTYPE_C32`, indicating `std::complex<float>` waveform samples.
When `collector.waveform` is `false`, the third multipart frame is present but
empty. Consumers should validate frame counts and sizes before decoding.

### Enabling test output

To check the sampler output before implementing your own consumer, you can set `printer.enable` to `yes` in the configuration file.
The sampler will then output something like below:

```csv
# Timestamp, RSSI, Mode-S message in hex format
...
2026-05-10T00:24:40.759419,-18.45 dB,8D4CAD49F8330002004ABCFB1932
2026-05-10T00:24:40.767487,-21.54 dB,8D88628EE11B3B000000004A5BA3
2026-05-10T00:24:40.782006,-22.11 dB,02E19013110C51
...
```

## License

The original source code in this repository is licensed under the MIT License.
See `LICENSE` and `NOTICE`.

## Acknowledgements

- This work was supported by JSPS KAKENHI Grant Number [JP25K03255](https://kaken.nii.ac.jp/en/grant/KAKENHI-PROJECT-25K03255/).
- The Mode-S decoding logic is based on [dump1090](https://github.com/antirez/dump1090), licensed under BSD 3-Clause License. See `NOTICE` for details.
