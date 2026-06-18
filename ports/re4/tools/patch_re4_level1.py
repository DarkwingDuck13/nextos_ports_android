#!/usr/bin/env python3
import argparse
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description="Patch RE4 MainMenu level1 scene")
    parser.add_argument("input", help="Input level1 asset")
    parser.add_argument("output", help="Output level1 asset")
    args = parser.parse_args()

    sys.path.insert(0, "/tmp/unitypy")
    import UnityPy  # type: ignore

    env = UnityPy.load(args.input)
    canvas_obj = next((o for o in env.objects if o.path_id == 296), None)
    if canvas_obj is None:
        raise RuntimeError("MainMenuCanvas Canvas object (path 296) not found")

    canvas = canvas_obj.read_typetree(check_read=False)
    if canvas.get("m_GameObject", {}).get("m_PathID") != 12:
        raise RuntimeError("Canvas path 296 is no longer MainMenuCanvas")

    canvas["m_RenderMode"] = 0  # ScreenSpaceOverlay
    canvas["m_Camera"] = {"m_FileID": 0, "m_PathID": 0}
    canvas["m_PlaneDistance"] = 0.0
    canvas_obj.save_typetree(canvas)

    with open(args.output, "wb") as fh:
        fh.write(canvas_obj.assets_file.save())

    print(f"patched {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
