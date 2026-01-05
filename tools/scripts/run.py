import argparse
import os
import platform
import subprocess
import sys
from pathlib import Path


def log(prefix: str, message: str) -> None:
    print(f"[{prefix}] {message}", flush=True)


def run(prefix: str, cmd, cwd: Path) -> None:
    log(prefix, " ".join(str(p) for p in cmd))
    subprocess.run(cmd, cwd=cwd, check=True)


def configure_and_build(root: Path, build_dir: Path, config: str) -> None:
    build_dir.mkdir(parents=True, exist_ok=True)
    cmake = os.environ.get("CMAKE", "cmake")
    run("build", [cmake, "-S", str(root), "-B", str(build_dir), f"-DCMAKE_BUILD_TYPE={config}"], root)

    cmd = [cmake, "--build", str(build_dir), "--target", "orderbook_backend"]
    if platform.system() == "Windows":
        cmd.extend(["--config", config])
    run("build", cmd, root)


def backend_binary(build_dir: Path, config: str) -> Path:
    exe = "orderbook_backend.exe" if platform.system() == "Windows" else "orderbook_backend"
    candidate = build_dir / exe
    if platform.system() == "Windows":
        nested = build_dir / config / exe
        if nested.exists():
            return nested
    return candidate


def cmd_backend(args) -> int:
    root = Path(__file__).resolve().parents[1]
    build_dir = (root / "build").resolve()
    configure_and_build(root, build_dir, args.config)
    bin_path = backend_binary(build_dir, args.config)

    if args.build_only:
        log("backend", f"built at {bin_path}")
        return 0

    cmd = [str(bin_path), "--symbol", args.symbol]
    log("backend", f"launching {bin_path}")
    return subprocess.call(cmd, cwd=root)


def cmd_frontend(args) -> int:
    root = Path(__file__).resolve().parents[1]
    build_dir = (root / "build").resolve()
    configure_and_build(root, build_dir, args.config)

    if not args.skip_pip:
        req = root / "gui" / "requirements.txt"
        if req.exists():
            run("frontend", [sys.executable, "-m", "pip", "install", "-r", str(req)], root)

    bin_path = backend_binary(build_dir, args.config)
    gui_script = root / "gui" / "main.py"
    cmd = [sys.executable, str(gui_script), "--symbol", args.symbol, "--backend-path", str(bin_path)]
    log("frontend", f"launching GUI {gui_script}")
    return subprocess.call(cmd, cwd=root)


def main() -> int:
    parser = argparse.ArgumentParser(description="Runner for Ghost (backend + PySide6 GUI)")
    subparsers = parser.add_subparsers(dest="cmd", required=True)

    p_backend = subparsers.add_parser("backend", help="Build and run only C++ backend")
    p_backend.add_argument("--config", default="Release")
    p_backend.add_argument("--symbol", default="BIOUSDT")
    p_backend.add_argument("--build-only", action="store_true")
    p_backend.set_defaults(func=cmd_backend)

    p_front = subparsers.add_parser("frontend", help="Build backend and launch GUI")
    p_front.add_argument("--config", default="Release")
    p_front.add_argument("--symbol", default="BIOUSDT")
    p_front.add_argument("--skip-pip", action="store_true")
    p_front.set_defaults(func=cmd_frontend)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
