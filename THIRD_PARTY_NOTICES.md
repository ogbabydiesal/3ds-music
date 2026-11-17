# Third-party notices

The eucseq and osc3ds apps include or are derived from the following third-party work. Ship this file, or an equivalent "About / Licenses" screen, with any build you distribute.

## STK (The Synthesis ToolKit in C++): the REVERB node

The REVERB node in `sequencer/source/synth.c` (`process_reverb`, `jc_lengths`) follows the structure, constants and delay lengths of STK's `JCRev` class.

```
The Synthesis ToolKit in C++ (STK)

Copyright (c) 1995-2023 Perry R. Cook and Gary P. Scavone

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

Any person wishing to distribute modifications to the Software is
asked to send the modifications to the original developer so that they
can be incorporated into the canonical version.  This is, however, not
a binding provision of this license.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## libctru, citro2d, citro3d (devkitPro)

These libraries are linked into both apps and are distributed under the **zlib license**. It requires no notice in binaries, but altered source must be marked as altered, and its origin must not be misrepresented.

## devkitARM toolchain runtime (newlib, libgcc, libm)

These are linked into both apps.

- **libgcc** is covered by the GCC Runtime Library Exception, which allows closed-source and paid programs.
- **newlib** is a collection of permissive (mostly BSD-style) licenses. Some of them ask that their copyright notices be reproduced in the documentation of binary distributions. The full list is in newlib's `COPYING.NEWLIB`, available from the newlib source (sourceware.org/newlib) and devkitPro.
