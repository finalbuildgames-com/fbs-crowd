# Third-party notices

The MIT license covers Final Build Games' original contributions. Third-party
components and adapted portions retain the terms reproduced below.

## third_party/flecs/LICENSE

MIT License

Copyright (c) 2025 Sander Mertens

Portions Copyright (c) Meta Platforms, Inc. and affiliates

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


## third_party/flecs/PROVENANCE.json

{
  "name": "flecs",
  "upstream": "https://github.com/SanderMertens/flecs",
  "release": "v4.1.6",
  "commit": "fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8",
  "archive": "https://github.com/SanderMertens/flecs/archive/refs/tags/v4.1.6.tar.gz",
  "archive_sha256": "29ccf56961b7ffbd38cce2227a06c0722c7df464422e86619a65ee37bb31bae7",
  "retrieved": "2026-09-25",
  "license": "MIT (LICENSE, unmodified)",
  "files": {
    "flecs_no_addons.c": {"from": "distr/flecs_no_addons.c", "sha256": "a710138a1f323ab809d61a90ddcaf118dadf17ce7dbf7d1849b4a70b85082338"},
    "flecs_no_addons.h": {"from": "distr/flecs_no_addons.h", "sha256": "4f88814699076131d227950cd5109eef739c279e03bc36c8ba5d830d0bf5813a"},
    "flecs.c": {"from": "distr/flecs.c", "sha256": "6005392eb13c0f3c7abdecb2f85271e50934f63919afebf0bbe85f6dfc7320d6"},
    "flecs.h": {"from": "distr/flecs.h", "sha256": "526036a5a41678e2a43a3cb835e9eaa70fd1993868b1978950c0d275752f69b1"},
    "LICENSE": {"from": "LICENSE", "sha256": "5f89f3edd5d1e458cd48509818e23b57d85869f7d57d0ebe31bcfd3a73e5dd60"}
  },
  "local_changes": "none"
}


Flecs 4.1.6 core-only amalgamation is unmodified, pinned at fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8, MIT. Binary distributions must include its license.
