# Third-party notices

Obtain the SDKs separately from their owners. Locally built ZIP packages include binaries
built against the OpenVR, OpenXR and NVAPI SDKs, along with this file.

## Valve OpenVR SDK

- Provides `openvr.h`, the interface definitions the layer compiles against.
- Licensed under BSD-3-Clause.
- Obtained from Valve's upstream repository. https://github.com/ValveSoftware/openvr
- Obtained separately. The layer in a locally built package is compiled
  against its headers; Valve's notice is reproduced below.

## NVIDIA NVAPI SDK

- Provides `nvapi.h` and the `nvapi64` static library for the VRS entry points
  `NvAPI_D3D11_CreateShadingRateResourceView`,
  `NvAPI_D3D11_RSSetShadingRateResourceView` and
  `NvAPI_D3D11_RSSetViewportsPixelShadingRates`.
- Licensed under MIT.
- Obtained from NVIDIA's upstream repository. https://github.com/NVIDIA/nvapi
- Obtained separately. The layer in a locally built package links the
  `nvapi64` static library; NVIDIA's notice is reproduced below. NVAPI itself is
  loaded from the installed driver at runtime, so no NVIDIA DLL is shipped.

## Khronos OpenXR SDK

- Used for the OpenXR API, D3D11 binding and loader negotiation headers.
- License for these headers is Apache-2.0 OR MIT. This project uses the MIT option.
- Obtained from https://github.com/KhronosGroup/OpenXR-SDK.
- The SDK is obtained separately for builds. The runtime's OpenXR loader is supplied
  by the application. Khronos's MIT notice is reproduced below.

## Microsoft Direct3D 11 and D3DCompiler

- Provides render-target APIs and runtime shader compilation for visual aids.
- These libraries are supplied by Windows. The package does not bundle their DLLs.

## Microsoft Visual C++ toolchain

- Used for building. The pinned version is in `cmake/pinned_msvc_toolset.txt`.
- Redistribution of a binary built with it is covered by the Visual Studio
  redistributable terms.

## Python

- Runs the clang-format wrapper `tools/format/format_cpp.py`.
- Python is licensed under the PSF License.
- The package does not contain a Python runtime.

## LLVM clang-format

- Used by the formatting wrapper at the version pinned in `tools/format/requirements.txt`.
- Licensed under Apache-2.0 WITH LLVM-exception.
- Downloaded for development. Locally built packages contain no clang-format binary.

## License texts

### OpenVR SDK

```text
Copyright (c) 2015, Valve Corporation
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### NVAPI SDK

The static library carries the 2024 copyright notice. The SDK headers also carry
Copyright (c) 2019-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved, under MIT.

```text
SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: MIT

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

### OpenXR SDK headers

```text
MIT License

Copyright (c) 2017-2026 The Khronos Group Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```
