"""Firefox's own lavapipe: the software Vulkan driver WebGPU uses on Linux,
built from pinned sources and our patches, shipped in the package.

    python3 scripts/ci/lavapipe_build.py --out DIR --clang DIR --sysroot DIR [--jobs N]
    python3 scripts/ci/lavapipe_build.py --key --clang DIR

Writes DIR/libvulkan_lvp.so and DIR/VERSION.json and prints the library's
path. A library already in DIR built from the same inputs (the workflow caches
it by --key) is used as it is.

Why: WebGPU on Linux only has wgpu's Vulkan backend. Without a Vulkan driver on
the host requestAdapter() answered null, where retail on the declared GPU has
an adapter, and with the host's Mesa its version decided limits and raster
bytes. The driver now comes with the engine: Mesa 26.2.6's lavapipe on LLVM
22.1.8 (the revision Mozilla's clang-22 is built from), compiled with Mozilla's
clang and sysroot, no Vulkan loader (the loader would bring the host's layers
and environment back), and patched (scripts/ci/lavapipe/*.patch) so that no
host value reaches it: every Mesa option compiled in and the environment never
read, one x86-64 profile (SSE to 4.2, AVX and F16C for half floats; never FMA,
AVX2 or AVX-512; 128-bit vectors) whatever the CPU, the exact
sqrt instead of a vendor-dependent rsqrt, a fixed heap, budget and thread
count, no /dev/udmabuf. It meets Firefox's glibc 2.17 floor the way Firefox
does, by defining __cxa_thread_atexit_impl (glibc 2.18) inside the library
(0005, the counterpart of build/unix/stdc++compat for a static libstdc++).
It advertises the storage limits it honours (0008: a 2^31-4 byte storage
buffer binding, and storage buffers and images bounded by its descriptor sets
rather than gallium's legacy 32 and 64 slots), and every build proves them by
using them (scripts/ci/lavapipe/proof) before the library is kept.
Firefox maps it under the loader's name (SONAME
libvulkan.so.1) and asks it for vkGetInstanceProcAddr directly
(dom/webgpu/StealthWebGPUDeclaration.cpp).

Linux only (Windows has WARP). Without the file WebGPU answers null (it never falls
back to a host driver).
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))  # upstream_toolchain.py

from upstream_toolchain import http_get  # noqa: E402

PATCHES = HERE / "lavapipe"
# A Vulkan program that uses, beyond what lavapipe used to advertise, the limits
# patch 0008 raises: every build runs it before the library is kept.
PROOF = PATCHES / "proof"
VERSIONS = PATCHES / "VERSIONS.json"
LIB = "libvulkan_lvp.so"
SONAME = "libvulkan.so.1"
EXPORTS = ("vkGetInstanceProcAddr", "vk_icdGetInstanceProcAddr")
# What the library may link against: the C runtime, nothing of the host's.
NEEDED_OK = {"libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
             "ld-linux-x86-64.so.2"}
# Firefox's own floors for x86_64 (python/mozbuild/mozbuild/action/check_binary.py).
FLOORS = {"GLIBC": (2, 17), "GLIBCXX": (3, 4, 19), "CXXABI": (1, 3, 7), "GCC": (4, 8)}

LLVM_CMAKE = [
    "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DLLVM_TARGETS_TO_BUILD=X86",
    "-DLLVM_ENABLE_PROJECTS=", "-DLLVM_ENABLE_RUNTIMES=", "-DBUILD_SHARED_LIBS=OFF",
    "-DLLVM_BUILD_LLVM_DYLIB=OFF", "-DLLVM_LINK_LLVM_DYLIB=OFF", "-DLLVM_ENABLE_PIC=ON",
    "-DLLVM_INCLUDE_TESTS=OFF", "-DLLVM_INCLUDE_EXAMPLES=OFF", "-DLLVM_INCLUDE_BENCHMARKS=OFF",
    "-DLLVM_INCLUDE_DOCS=OFF", "-DLLVM_ENABLE_ZLIB=OFF", "-DLLVM_ENABLE_ZSTD=OFF",
    "-DLLVM_ENABLE_LIBXML2=OFF", "-DLLVM_ENABLE_LIBEDIT=OFF", "-DLLVM_ENABLE_LIBPFM=OFF",
    "-DLLVM_ENABLE_Z3_SOLVER=OFF", "-DLLVM_ENABLE_BINDINGS=OFF", "-DLLVM_APPEND_VC_REV=OFF",
    "-DLLVM_ENABLE_ASSERTIONS=OFF", "-DLLVM_INSTALL_UTILS=OFF",
    # Without RTTI, and Mesa told so (cpp_rtti=false below): meson refuses a
    # mismatch (run 41330839671).
    "-DLLVM_ENABLE_RTTI=OFF",
    "-DLLVM_DISTRIBUTION_COMPONENTS=llvm-headers;llvm-libraries;llvm-config;cmake-exports",
]

MESON_OPTIONS = [
    "-Dbuildtype=release", "-Db_ndebug=true",
    "-Dvulkan-drivers=swrast", "-Dgallium-drivers=", "-Dplatforms=",
    "-Dglx=disabled", "-Degl=disabled", "-Dgbm=disabled", "-Dopengl=false",
    "-Dgles1=disabled", "-Dgles2=disabled", "-Dglvnd=disabled",
    "-Dllvm=enabled", "-Dshared-llvm=disabled", "-Ddraw-use-llvm=true", "-Dcpp_rtti=false",
    "-Dshader-cache=disabled", "-Dxmlconfig=disabled", "-Dexpat=disabled",
    "-Dzstd=disabled", "-Dzlib=disabled", "-Dlibunwind=disabled", "-Dvalgrind=disabled",
    "-Dlmsensors=disabled", "-Dperfetto=false", "-Dbuild-tests=false", "-Dtools=",
    "-Dvulkan-layers=", "-Dvideo-codecs=", "-Dgallium-rusticl=false", "-Dteflon=false",
    "-Dspirv-tools=disabled",
    # libdrm is mandatory once a Vulkan driver is on: Mesa's own pinned wrap,
    # static, so the host's libdrm.so.2 is never needed.
    "-Dallow-fallback-for=libdrm", "--force-fallback-for=libdrm",
    "-Dlibdrm:default_library=static",
]


def version_tuple(text: str) -> tuple:
    return tuple(int(x) for x in text.split("."))


def inputs_key(clang_version: str, versions: Path = VERSIONS, patches: Path = PATCHES,
               script: Path = Path(__file__)) -> str:
    """Everything the library is built from: pins, patches, this script and the
    compiler. The workflow caches the library under it."""
    h = hashlib.sha256()
    h.update(versions.read_bytes())
    for p in sorted(patches.glob("*.patch")):
        h.update(p.name.encode() + b"\0" + p.read_bytes())
    for p in sorted((patches / "proof").glob("*")):
        h.update(b"proof/" + p.name.encode() + b"\0" + p.read_bytes())
    h.update(script.read_bytes())
    h.update(clang_version.encode())
    return h.hexdigest()[:32]


def check_binary(dynamic: str, symbols: str) -> list:
    """The library's problems, as sentences, from `readelf -d` and
    `readelf --dyn-syms -W`: what it links against, the loader's name, the two
    entry points, and Firefox's symbol-version floors."""
    found = []
    needed = set(re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", dynamic))
    extra = sorted(needed - NEEDED_OK)
    if extra:
        found.append("links against %s, which the host would have to provide" % ", ".join(extra))
    soname = re.findall(r"\(SONAME\)\s+Library soname: \[([^\]]+)\]", dynamic)
    if soname != [SONAME]:
        found.append("SONAME is %s, not %s: Firefox maps it under the loader's name" % (soname, SONAME))
    defined = set()
    for line in symbols.splitlines():
        cols = line.split()
        if len(cols) >= 8 and cols[4] == "GLOBAL" and cols[6] != "UND":
            defined.add(cols[7].split("@")[0])
    for name in EXPORTS:
        if name not in defined:
            found.append("does not export %s" % name)
    # Imports above Firefox's floors, named, as check_binary.py checks them:
    # undefined symbols only.
    above = {}
    for line in symbols.splitlines():
        cols = line.split()
        if len(cols) < 8 or cols[6] != "UND" or "@" not in cols[7]:
            continue
        name, _, version = cols[7].partition("@")
        version = version.lstrip("@")
        for prefix, floor in FLOORS.items():
            number = version[len(prefix) + 1:]
            if (version.startswith(prefix + "_") and re.fullmatch(r"[0-9]+(\.[0-9]+)*", number)
                    and version_tuple(number) > floor):
                above.setdefault(version, set()).add(name)
    for version in sorted(above):
        prefix = version.partition("_")[0]
        found.append("needs %s (%s), above Firefox's floor %s"
                     % (version, ", ".join(sorted(above[version])), ".".join(map(str, FLOORS[prefix]))))
    return found


def clang_version(clang: Path, run=subprocess.run) -> str:
    out = run([str(clang / "bin" / "clang"), "--version"], check=True, capture_output=True, text=True)
    return out.stdout.splitlines()[0].strip()


def glslang_version(text: str) -> tuple:
    """`Glslang Version: 11:14.0.0` (API:release) or a bare release."""
    m = re.search(r"Glslang Version:\s*[0-9]+:([0-9]+)\.([0-9]+)", text) or re.search(r"([0-9]+)\.([0-9]+)\.[0-9]+", text)
    if not m:
        raise RuntimeError("glslangValidator --version says nothing readable: %r" % text)
    return int(m.group(1)), int(m.group(2))


def meson_list(items) -> str:
    """A meson machine-file array: single-quoted strings."""
    return "[%s]" % ", ".join("'%s'" % str(i).replace("\\", "\\\\").replace("'", "\\'") for i in items)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


PROOF_LINES = ("A storage buffer binding of 2147483644 bytes",
               "B fragment shader with 64 storage buffers and 64 storage images")


def proof_problems(output: str) -> list:
    """What the proof's output says is wrong; [] when every use worked."""
    problems = []
    for start in PROOF_LINES:
        line = next((l for l in output.splitlines() if l.startswith(start)), None)
        if line is None:
            problems.append("the proof never reported: %s" % start)
        elif not line.endswith("-> OK"):
            problems.append(line)
    return problems


def prove(lib: Path, vulkan_include: Path, work: Path, run=subprocess.run) -> list:
    """Builds scripts/ci/lavapipe/proof against `lib` and runs it: a 2^31-4
    byte storage buffer binding written at its last u32, and a fragment shader
    writing 64 storage buffers and 64 storage images (run 41372399866: our
    lavapipe advertised 1 << 27 and 104, gallium's legacy slots, under the
    declared retail limits). Returns the problems."""
    work.mkdir(parents=True, exist_ok=True)
    for shader in ("big.comp", "many.vert", "many.frag"):
        run(["glslangValidator", "-V", "--target-env", "vulkan1.3", "-o", str(work / (shader + ".spv")),
             str(PROOF / shader)], check=True, capture_output=True)
    exe = work / "proof"
    run(["cc", "-O1", "-I", str(vulkan_include), "-o", str(exe), str(PROOF / "proof.c"), "-ldl"], check=True)
    done = run([str(exe), str(lib)], cwd=work, capture_output=True, text=True)
    print(done.stdout, end="")
    return proof_problems(done.stdout) + ([] if done.returncode == 0 else ["the proof exited %d" % done.returncode])


def cached(out: Path, key: str) -> Path | None:
    lib, ver = out / LIB, out / "VERSION.json"
    if lib.is_file() and ver.is_file():
        record = json.loads(ver.read_text("utf-8"))
        if record.get("inputs") == key and record.get("sha256") == sha256(lib):
            return lib
    return None


def build(out: Path, clang: Path, sysroot: Path, jobs: int, run=subprocess.run, get=http_get) -> Path:
    versions = json.loads(VERSIONS.read_text("utf-8"))
    key = inputs_key(clang_version(clang, run))
    hit = cached(out, key)
    if hit:
        return hit
    glslang = run(["glslangValidator", "--version"], check=True, capture_output=True, text=True).stdout
    if glslang_version(glslang) < tuple(versions["glslang_min"]):
        raise RuntimeError("glslangValidator older than %s: lavapipe's shaders need it" % versions["glslang_min"])
    series = sorted(PATCHES.glob("*.patch"))
    if not series:
        raise ValueError("no patches in %s: a lavapipe without them reads the host" % PATCHES)
    cc, cxx = str(clang / "bin" / "clang"), str(clang / "bin" / "clang++")
    env = dict(os.environ, SOURCE_DATE_EPOCH="1767225600")
    # Nothing of the runner's: pkg-config sees the sysroot only.
    env["PKG_CONFIG_LIBDIR"] = ":".join(str(sysroot / d) for d in (
        "usr/lib/x86_64-linux-gnu/pkgconfig", "usr/lib/pkgconfig", "usr/share/pkgconfig"))
    env["PKG_CONFIG_SYSROOT_DIR"] = str(sysroot)
    for var in [v for v in env if v.startswith(("LP_", "GALLIUM_", "GALLIVM_", "MESA_", "VK_"))]:
        env.pop(var)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        # Mesa's build tools, pinned.
        venv = tmp / "venv"
        run([sys.executable, "-m", "venv", str(venv)], check=True)
        pip = [str(venv / "bin" / "pip"), "install", "--quiet", "--disable-pip-version-check"]
        run(pip + ["%s==%s" % kv for kv in versions["pip"].items()], check=True)
        env["PATH"] = "%s:%s" % (venv / "bin", env["PATH"])

        # LLVM at the pinned commit, static, X86 only.
        llvm_src, llvm_build, llvm = tmp / "llvm-project", tmp / "llvm-build", tmp / "llvm"
        llvm_src.mkdir()
        git = ["git", "-c", "core.autocrlf=false", "-C", str(llvm_src)]
        run(git + ["init", "--quiet"], check=True)
        run(git + ["fetch", "--quiet", "--depth", "1", versions["llvm"]["repo"], versions["llvm"]["commit"]],
            check=True)
        run(git + ["checkout", "--quiet", "FETCH_HEAD"], check=True)
        head = run(git + ["rev-parse", "HEAD"], check=True, capture_output=True, text=True).stdout.strip()
        if head != versions["llvm"]["commit"]:
            raise RuntimeError("LLVM checkout is %s, not the pinned %s" % (head, versions["llvm"]["commit"]))
        run(["cmake", "-S", str(llvm_src / "llvm"), "-B", str(llvm_build)] + LLVM_CMAKE + [
            "-DCMAKE_C_COMPILER=%s" % cc, "-DCMAKE_CXX_COMPILER=%s" % cxx,
            "-DCMAKE_SYSROOT=%s" % sysroot, "-DCMAKE_INSTALL_PREFIX=%s" % llvm,
            "-DCMAKE_C_FLAGS=-ffile-prefix-map=%s=." % tmp, "-DCMAKE_CXX_FLAGS=-ffile-prefix-map=%s=." % tmp,
        ], check=True, env=env)
        run(["ninja", "-C", str(llvm_build), "-j", str(jobs), "install-distribution"], check=True, env=env)

        # Mesa from the pinned tarball, checked, with our patches.
        tarball = get(versions["mesa"]["url"])
        got = hashlib.sha256(tarball).hexdigest()
        if got != versions["mesa"]["sha256"]:
            raise RuntimeError("Mesa tarball sha256 %s, pinned %s" % (got, versions["mesa"]["sha256"]))
        with tarfile.open(fileobj=io.BytesIO(tarball), mode="r:xz") as tar:
            tar.extractall(tmp, filter="data")
        mesa = tmp / ("mesa-%s" % versions["mesa"]["version"])
        for patch in series:
            # --check first: a patch that no longer applies is a refusal, never
            # a library built without it.
            run(["git", "apply", "--check", str(patch)], check=True, cwd=mesa)
            run(["git", "apply", str(patch)], check=True, cwd=mesa)
        native = tmp / "native.ini"
        flags = ["--sysroot=%s" % sysroot, "-ffile-prefix-map=%s=." % tmp]
        native.write_text("\n".join([
            "[binaries]",
            "c = %s" % meson_list([cc] + flags),
            "cpp = %s" % meson_list([cxx] + flags),
            "llvm-config = %s" % meson_list([llvm / "bin" / "llvm-config"]),
            "[built-in options]",
            # LLVM is C++: its runtime goes into the library, never the host's.
            "c_link_args = %s" % meson_list(["--sysroot=%s" % sysroot, "-static-libstdc++", "-static-libgcc"]),
            "cpp_link_args = %s" % meson_list(["--sysroot=%s" % sysroot, "-static-libstdc++", "-static-libgcc"]),
            "",
        ]), "utf-8")
        mesa_build = tmp / "mesa-build"
        run(["meson", "setup", str(mesa_build), str(mesa), "--native-file", str(native),
             "--wrap-mode=default"] + MESON_OPTIONS, check=True, env=env)
        run(["ninja", "-C", str(mesa_build), "-j", str(jobs), "src/gallium/targets/lavapipe/%s" % LIB],
            check=True, env=env)

        out.mkdir(parents=True, exist_ok=True)
        lib = out / LIB
        shutil.copy2(mesa_build / "src" / "gallium" / "targets" / "lavapipe" / LIB, lib)
        run(["strip", "--strip-unneeded", str(lib)], check=True)
        run(["patchelf", "--set-soname", SONAME, str(lib)], check=True)
        dynamic = run(["readelf", "-d", str(lib)], check=True, capture_output=True, text=True).stdout
        symbols = run(["readelf", "--dyn-syms", "-W", str(lib)], check=True, capture_output=True, text=True).stdout
        problems = check_binary(dynamic, symbols)
        if not problems:
            problems = prove(lib, mesa / "include", tmp / "proof", run)
        if problems:
            lib.unlink()
            raise RuntimeError("the lavapipe library is not shippable:\n  " + "\n  ".join(problems))
        (out / "VERSION.json").write_text(json.dumps({
            "mesa": versions["mesa"]["version"], "llvm": versions["llvm"]["commit"],
            "patches": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in series},
            "compiler": clang_version(clang, run), "inputs": key, "sha256": sha256(lib),
        }, indent=1, sort_keys=True) + "\n", "utf-8")
    return lib


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--clang", type=Path, default=Path.home() / ".mozbuild" / "clang")
    ap.add_argument("--sysroot", type=Path, default=Path.home() / ".mozbuild" / "sysroot-x86_64-linux-gnu")
    ap.add_argument("--out", type=Path)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--key", action="store_true", help="print the inputs key and exit")
    args = ap.parse_args(argv)
    if args.key:
        print(inputs_key(clang_version(args.clang)))
        return 0
    if not args.out:
        ap.error("--out is required")
    print(build(args.out, args.clang, args.sysroot, args.jobs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
