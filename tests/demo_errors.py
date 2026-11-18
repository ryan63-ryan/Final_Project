"""Extra file-input checks for the storage demo; uses Python's standard library."""
import pathlib
import subprocess
import sys
import tempfile

program = pathlib.Path(sys.argv[1]).resolve()
cases = [
    ("negative-count", "-1\n", "prompt length"),
    ("too-many", "8\n", "prompt length"),
    ("bad-token", "1\n256\n0\n", "byte value"),
    ("missing-token", "2\n1\n", "byte value"),
    ("missing-append", "1\n1\n", "append byte"),
    ("negative-append", "0\n-1\n", "append byte"),
    ("extra-input", "0\n1 x\n", "extra input"),
    ("invalid-header", "abc\n", "prompt length"),
]
with tempfile.TemporaryDirectory() as directory:
    folder = pathlib.Path(directory)
    for name, content, message in cases:
        path = folder / name
        path.write_text(content)
        result = subprocess.run([str(program), str(path)], capture_output=True, text=True)
        assert result.returncode == 1 and message in result.stderr, (name, result)
    for args in [[], [str(folder / "missing")]]:
        result = subprocess.run([str(program)] + args, capture_output=True, text=True)
        assert result.returncode == 1 and result.stderr.startswith("error:"), result
    for name, content, expected in [
        ("empty", "0\n8\n", "child blocks=[0] values=[8]"),
        ("pressure", "7\n1 2 3 4 5 6 7\n8\n", "append=NO_CAPACITY copies=0"),
    ]:
        path = folder / name
        path.write_text(content)
        result = subprocess.run([str(program), str(path)], capture_output=True, text=True)
        assert result.returncode == 0 and expected in result.stdout and not result.stderr, (name, result)
print("PASS 12 demo boundary/error cases")
