# Third-party notices

The frame-generation pass structure in hook/framegen.cpp and the shader resource IDs in common/shader_extract.cpp are derived from lsfg-vk v1 (https://github.com/PancakeTAS/lsfg-vk), used under the MIT licence:

```
## MIT License

Copyright (c) 2025 lsfg-vk

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## MinHook

NitroHook.dll statically links MinHook (https://github.com/TsudaKageyu/minhook), BSD 2-Clause licence, Copyright (C) 2009-2017 Tsuda Kageyu. Licence text: https://github.com/TsudaKageyu/minhook/blob/master/LICENSE.txt

## Vulkan-Headers

NitroVk.dll is built with the Khronos Vulkan headers (https://github.com/KhronosGroup/Vulkan-Headers), Apache License 2.0.

## Lossless Scaling

Lossless Scaling and its shaders are the property of their developer. This project ships none of them; it reads them from the user's own Lossless.dll at runtime.
